#include "can_link.h"

#ifdef CAN_VIA_LINK

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

#include "../../devboard/hal/hal.h"
#include "../../devboard/hal/hw_miv_esp32eth_pins.h"
#include "../../devboard/utils/logging.h"
#include "../link/be_link.h"

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
  struct be_link_can m = {};
  m.iface = iface;
  m.flags = BE_LINK_CAN_TX | (frame.ext_ID ? BE_LINK_CAN_EXT : 0) |
            (frame.FD ? (BE_LINK_CAN_FD | BE_LINK_CAN_BRS) : 0);
  m.len = frame.DLC > 64 ? 64 : frame.DLC;
  m.id = frame.ID;
  memcpy(m.data, frame.data.u8, m.len);
  return link_send(BE_LINK_CAN, &m, be_link_can_wire_size(m.len));
}

void can_link_poll(void (*on_frame)(uint8_t iface, CAN_frame* frame)) {
  if (!started) {
    return;
  }
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
        } break;
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
