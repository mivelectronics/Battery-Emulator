#include "can_link.h"

#ifdef CAN_VIA_LINK

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

#include "../../devboard/hal/hal.h"
#include "../../devboard/hal/hw_miv_esp32eth_pins.h"
#include "../../devboard/utils/logging.h"
#include "../link/be_link.h"
#include "../link/be_snapshot.h"
#include "../link/s32k_ota.h"

static constexpr uint32_t LINK_BAUD = 2000000;
static bool started = false;
static uint8_t tx_seq = 0;
static struct be_link_rx rx;
static unsigned long last_hello_ms = 0;
static bool hello_seen = false;

static bool link_send(uint8_t type, const void* payload, uint16_t len) {
  static uint8_t enc[BE_LINK_MAX_ENCODED];
  size_t n = be_link_encode(type, tx_seq++, payload, len, enc, sizeof(enc));
  if (n == 0 || (size_t)Serial1.availableForWrite() < n) {
    return false;
  }
  return Serial1.write(enc, n) == n;
}

bool can_link_send_raw(uint8_t type, const void* payload, uint16_t len) {
  return started && link_send(type, payload, len);
}

bool can_link_begin() {
  if (started) {
    return true;
  }
  using namespace miv_esp32eth;
  if (!esp32hal->alloc_pins("CAN link", LINK_TX, LINK_RX)) {
    return false;
  }
  Serial1.setRxBufferSize(8192);
  Serial1.setTxBufferSize(4096);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  be_link_rx_init(&rx);
  started = true;

  struct be_link_hello h = {};
  h.proto_version = BE_LINK_PROTO_VERSION;
  h.role = BE_LINK_ROLE_NET;
  strncpy(h.fw_version, "esp32-be", sizeof(h.fw_version) - 1);
  link_send(BE_LINK_HELLO, &h, sizeof(h));
  logging.println("CAN link to S32K started (UART1, 2 Mbit/s)");
  return true;
}

bool can_link_configure(uint8_t iface, uint32_t nominal_kbps, uint32_t data_kbps) {
  struct be_link_cmd c = {};
  c.code = 1;  // set bit rate
  c.arg0 = (uint32_t)iface | (nominal_kbps << 8);
  c.arg1 = data_kbps;
  return link_send(BE_LINK_CMD, &c, sizeof(c));
}

bool can_link_send(uint8_t iface, const CAN_frame& frame) {
  if (s32k_ota_active()) {
    return false;  // the link stays quiet while the S32K takes an update
  }
  struct be_link_can m = {};
  m.iface = iface;
  m.flags = BE_LINK_CAN_TX | (frame.ext_ID ? BE_LINK_CAN_EXT : 0) |
            (frame.FD ? (BE_LINK_CAN_FD | BE_LINK_CAN_BRS) : 0);
  m.len = frame.DLC > 64 ? 64 : frame.DLC;
  m.id = frame.ID;
  memcpy(m.data, frame.data.u8, m.len);
  return link_send(BE_LINK_CAN, &m, be_link_can_wire_size(m.len));
}

#ifdef LINK_SELFTEST
#ifndef LINK_SELFTEST_PERIOD_MS
#define LINK_SELFTEST_PERIOD_MS 100
#endif
// Development test of the whole path without a CAN peer: the S32K is asked to put its channels in
// internal loopback, frames sent from here come back as received frames and are checked.
static uint32_t st_sent, st_echoed, st_bad;
static uint32_t st_seq;
static unsigned long st_next_ms, st_next_report_ms;
static bool st_armed;

static void selftest_tx() {
  const unsigned long now = millis();
  if (!can_link_alive() || (long)(now - st_next_ms) < 0) {
    return;
  }
  st_next_ms = now + LINK_SELFTEST_PERIOD_MS;
  if (!st_armed) {
    for (uint8_t i = 0; i < 3; i++) {
      struct be_link_cmd c = {};
      c.code = 2;  // loopback on
      c.arg0 = i;
      c.arg1 = 1;
      link_send(BE_LINK_CMD, &c, sizeof(c));
    }
    st_armed = true;
    st_next_ms = now + 500;
    return;
  }
  for (uint8_t i = 0; i < 3; i++) {
    CAN_frame f = {};
    f.FD = i != 0;
    f.DLC = i == 0 ? 8 : 64;
    f.ID = 0x100 + i;
    for (int k = 0; k < f.DLC; k++) {
      f.data.u8[k] = (uint8_t)(st_seq + k * 3);
    }
    if (can_link_send(i, f)) {
      st_sent++;
    }
  }
  st_seq++;
}

static void selftest_rx(const CAN_frame& f, uint8_t iface) {
  bool ok = f.ID == 0x100u + iface && f.DLC == (iface == 0 ? 8 : 64) && f.FD == (iface != 0);
  // The first byte carries the low 8 bits of the sequence, the rest must follow it
  for (int k = 1; ok && k < f.DLC; k++) {
    ok = f.data.u8[k] == (uint8_t)(f.data.u8[0] + k * 3);
  }
  if (ok) {
    st_echoed++;
  } else {
    st_bad++;
  }
}

static void selftest_report() {
  const unsigned long now = millis();
  if ((long)(now - st_next_report_ms) < 0) {
    return;
  }
  st_next_report_ms = now + 5000;
  logging.printf("LINK SELFTEST: peer %d sent %lu echoed %lu bad %lu | rx ok %lu crc %lu framing %lu overflow %lu\n",
                 can_link_alive(), (unsigned long)st_sent, (unsigned long)st_echoed, (unsigned long)st_bad,
                 (unsigned long)rx.stats.frames_ok, (unsigned long)rx.stats.crc_errors,
                 (unsigned long)rx.stats.framing_errors, (unsigned long)rx.stats.overflows);
}
#endif  // LINK_SELFTEST

void can_link_poll(void (*on_frame)(uint8_t iface, CAN_frame* frame)) {
  if (!started) {
    return;
  }
  // The S32K watches these to know a network module is present
  static unsigned long next_hello_ms = 0;
  if ((long)(millis() - next_hello_ms) >= 0) {
    next_hello_ms = millis() + 1000;
    struct be_link_hello h = {};
    h.proto_version = BE_LINK_PROTO_VERSION;
    h.role = BE_LINK_ROLE_NET;
    strncpy(h.fw_version, "esp32-be", sizeof(h.fw_version) - 1);
    link_send(BE_LINK_HELLO, &h, sizeof(h));
  }
#ifdef LINK_SELFTEST
  selftest_tx();
  selftest_report();
#endif
  s32k_ota_poll(millis());
  uint8_t buf[256];
  int avail;
  while ((avail = Serial1.available()) > 0) {
    size_t n = Serial1.readBytes(buf, avail > (int)sizeof(buf) ? sizeof(buf) : (size_t)avail);
    for (size_t i = 0; i < n; i++) {
      struct be_link_frame f;
      if (!be_link_rx_byte(&rx, buf[i], &f)) {
        continue;
      }
      switch (f.type) {
        case BE_LINK_CAN: {
          struct be_link_can m = {};
          memcpy(&m, f.payload, f.len < sizeof(m) ? f.len : sizeof(m));
          if (m.flags & BE_LINK_CAN_TX) {
            break;  // echo of our own transmission
          }
          CAN_frame cf = {};
          cf.FD = (m.flags & BE_LINK_CAN_FD) != 0;
          cf.ext_ID = (m.flags & BE_LINK_CAN_EXT) != 0;
          cf.DLC = m.len > 64 ? 64 : m.len;
          cf.ID = m.id;
          memcpy(cf.data.u8, m.data, cf.DLC);
#ifdef LINK_SELFTEST
          selftest_rx(cf, m.iface);
#endif
          on_frame(m.iface, &cf);
        } break;
        case BE_LINK_HELLO: {
          struct be_link_hello h = {};
          memcpy(&h, f.payload, f.len < sizeof(h) ? f.len : sizeof(h));
          if (!hello_seen) {
            h.fw_version[sizeof(h.fw_version) - 1] = 0;
            logging.printf("CAN link: S32K '%s', protocol %u\n", h.fw_version, h.proto_version);
            hello_seen = true;
          }
          last_hello_ms = millis();
          be_remote_on_hello(h.reserved, h.layout_hash);
        } break;
        case BE_LINK_SNAPSHOT:
          be_remote_on_snapshot(f.payload, f.len);
          break;
        case BE_LINK_SETTINGS:
          be_remote_on_settings(f.payload, f.len);
          break;
        case BE_LINK_ACK:
          if (f.len >= sizeof(struct be_link_ack)) {
            struct be_link_ack a;
            memcpy(&a, f.payload, sizeof(a));
            if (a.acked_type == BE_LINK_OTA) {
              s32k_ota_on_ack(f.payload, f.len);
            } else {
              be_remote_on_ack(a.acked_type, a.acked_seq, a.status);
            }
          }
          break;
        case BE_LINK_LOG: {
          char text[161];
          size_t len = f.len < 160 ? f.len : 160;
          memcpy(text, f.payload, len);
          text[len] = 0;
          logging.printf("S32K: %s\n", text);
        } break;
        default:
          break;
      }
    }
  }
}

bool can_link_alive() {
  return hello_seen && (millis() - last_hello_ms) < 3000;
}

#endif  // CAN_VIA_LINK
