#include "s32k_ota.h"

#ifdef CAN_VIA_LINK

#include <Arduino.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <stdlib.h>
#include <string.h>

#include "../../datalayer/datalayer.h"
#include "../../devboard/utils/logging.h"
#include "../can/can_link.h"
#include "be_link.h"
#include "be_snapshot.h"

static constexpr size_t RING_SIZE = 16384;  // four pieces of the page
static constexpr uint32_t IMAGE_MAGIC = 0x96f3b83du;
static constexpr int DATA_TRIES = 25;            // 25 x 200 ms: 5 s without an answer
static constexpr uint32_t FINISH_RETRY_MS = 3000;  // the check reads the whole slot back
static constexpr int FINISH_TRIES = 5;
static constexpr uint32_t STALL_MS = 10000;        // the S32K gives up after 10 s of silence too
static constexpr uint32_t QUIET_AFTER_DONE_MS = 5000;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Shared with the web task (under mux)
static s32k_ota_state st;
static uint8_t* ring = nullptr;
static size_t ring_read = 0, ring_count = 0;  // ring_read: the byte at st.written
static bool finish_requested = false, abort_requested = false;
static uint32_t start_ms = 0, last_piece_ms = 0, done_ms = 0;
static uint8_t head[32];

// Core task only
static uint32_t image_id = 0;
static bool inflight = false;
static uint8_t frame_id = 0;
static uint8_t frame_op = 0;
static uint8_t frame[sizeof(be_link_ota_hdr) + BE_LINK_OTA_DATA_MAX];
static uint16_t frame_len = 0;
static uint32_t frame_bytes = 0;  // image bytes in the DATA frame in flight
static uint32_t sent_ms = 0;
static int tries = 0;

static void fail(int error) {
  // under mux
  st.phase = S32K_OTA_FAILED;
  st.error = (int16_t)error;
  st.elapsed_ms = millis() - start_ms;
  inflight = false;
}

static bool phase_runs(uint8_t p) {
  return p == S32K_OTA_STARTING || p == S32K_OTA_SENDING || p == S32K_OTA_CHECKING;
}

bool s32k_ota_active() {
  portENTER_CRITICAL(&mux);
  const bool on = phase_runs(st.phase) || (st.phase == S32K_OTA_DONE && millis() - done_ms < QUIET_AFTER_DONE_MS);
  portEXIT_CRITICAL(&mux);
  return on;
}

// Section 7 of OTA_PLAN.md for the local core (bridge mode); with the core on the S32K, the S32K checks
static int local_safety() {
  if (be_remote_active()) {
    return 0;
  }
  if (datalayer.system.status.contactors_engaged != 0) {
    return BE_OTA_E_CONTACTORS;
  }
  if (datalayer.battery.status.max_charge_power_W != 0 || datalayer.battery.status.max_discharge_power_W != 0) {
    return BE_OTA_E_NOT_PAUSED;
  }
  const int32_t dA = datalayer.battery.status.current_dA;
  if (dA >= 10 || dA <= -10) {
    return BE_OTA_E_CURRENT;
  }
  return 0;
}

int s32k_ota_begin(const uint8_t* h, size_t head_len, uint32_t total) {
  if (!can_link_alive()) {
    return S32K_OTA_E_NO_LINK;
  }
  if (s32k_ota_active()) {
    return S32K_OTA_E_BUSY;
  }
  if (head_len < sizeof(head)) {
    return BE_OTA_E_NOT_IMAGE;
  }
  if (h[0] == 0xE9) {
    return S32K_OTA_E_ESP32_FILE;
  }
  uint32_t magic;
  memcpy(&magic, h, sizeof(magic));
  if (magic != IMAGE_MAGIC) {
    return BE_OTA_E_NOT_IMAGE;
  }
  const int safety = local_safety();
  if (safety != 0) {
    return safety;
  }
  if (ring == nullptr) {
    ring = (uint8_t*)malloc(RING_SIZE);
    if (ring == nullptr) {
      return S32K_OTA_E_MEMORY;
    }
  }
  portENTER_CRITICAL(&mux);
  memset(&st, 0, sizeof(st));
  memcpy(head, h, sizeof(head));
  st.total = total;
  const uint16_t rev = (uint16_t)(h[22] | (h[23] << 8));
  const uint32_t build = (uint32_t)h[24] | ((uint32_t)h[25] << 8) | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
  if (build != 0) {
    snprintf(st.version, sizeof(st.version), "%u.%u.%u+%lu", h[20], h[21], rev, (unsigned long)build);
  } else {
    snprintf(st.version, sizeof(st.version), "%u.%u.%u", h[20], h[21], rev);
  }
  ring_read = ring_count = 0;
  finish_requested = abort_requested = false;
  start_ms = last_piece_ms = millis();
  st.phase = S32K_OTA_STARTING;
  portEXIT_CRITICAL(&mux);
  logging.printf("S32K update %s, %lu bytes: starting\n", st.version, (unsigned long)total);
  return 0;
}

int s32k_ota_chunk(uint32_t offset, const uint8_t* data, size_t len) {
  int ret = 0;
  portENTER_CRITICAL(&mux);
  if (!phase_runs(st.phase) || finish_requested) {
    ret = st.error != 0 ? st.error : S32K_OTA_E_ABORTED;
  } else if (offset != st.received || offset + len > st.total) {
    ret = S32K_OTA_E_OFFSET;
  } else if (RING_SIZE - ring_count < len) {
    ret = -EAGAIN;
  } else {
    size_t w = (ring_read + ring_count) % RING_SIZE;
    for (size_t i = 0; i < len; i++) {
      ring[w] = data[i];
      w = (w + 1) % RING_SIZE;
    }
    ring_count += len;
    st.received += len;
    last_piece_ms = millis();
  }
  portEXIT_CRITICAL(&mux);
  return ret;
}

size_t s32k_ota_room() {
  portENTER_CRITICAL(&mux);
  const size_t room = ring != nullptr ? RING_SIZE - ring_count : 0;
  portEXIT_CRITICAL(&mux);
  return room;
}

int s32k_ota_finish() {
  int ret = 0;
  portENTER_CRITICAL(&mux);
  if (!phase_runs(st.phase)) {
    ret = st.error != 0 ? st.error : S32K_OTA_E_ABORTED;
  } else if (st.received != st.total) {
    ret = S32K_OTA_E_OFFSET;
  } else {
    finish_requested = true;
  }
  portEXIT_CRITICAL(&mux);
  return ret;
}

void s32k_ota_abort() {
  portENTER_CRITICAL(&mux);
  if (phase_runs(st.phase)) {
    abort_requested = true;
  }
  portEXIT_CRITICAL(&mux);
}

void s32k_ota_get(s32k_ota_state* out) {
  portENTER_CRITICAL(&mux);
  *out = st;
  if (phase_runs(st.phase)) {
    out->elapsed_ms = millis() - start_ms;
  }
  portEXIT_CRITICAL(&mux);
}

static void send_frame(uint32_t now_ms) {
  sent_ms = now_ms;
  tries++;
  if (tries > 1) {
    st.repeats++;
  }
  can_link_send_raw(BE_LINK_OTA, frame, frame_len);
}

// Best effort, no wait: the S32K also gives up on its own after 10 s of silence
static void send_abort() {
  be_link_ota_hdr h = {};
  h.op = BE_OTA_ABORT;
  h.id = ++frame_id;
  h.image_id = image_id;
  h.total = st.total;
  can_link_send_raw(BE_LINK_OTA, &h, sizeof(h));
}

static void new_frame(uint8_t op, uint32_t offset, uint32_t now_ms) {
  be_link_ota_hdr h = {};
  if (++frame_id == 0) {
    frame_id = 1;
  }
  h.op = op;
  h.id = frame_id;
  h.image_id = image_id;
  h.total = st.total;
  h.offset = offset;
  memcpy(frame, &h, sizeof(h));
  frame_len = sizeof(h);
  frame_op = op;
  inflight = true;
  tries = 0;
  send_frame(now_ms);
}

void s32k_ota_poll(uint32_t now_ms) {
  portENTER_CRITICAL(&mux);
  const uint8_t phase = st.phase;
  const bool abort_now = abort_requested;
  portEXIT_CRITICAL(&mux);
  if (!phase_runs(phase)) {
    return;
  }
  if (abort_now) {
    send_abort();
    portENTER_CRITICAL(&mux);
    fail(S32K_OTA_E_ABORTED);
    portEXIT_CRITICAL(&mux);
    logging.println("S32K update aborted");
    return;
  }

  if (inflight) {
    const uint32_t retry = frame_op == BE_OTA_FINISH ? FINISH_RETRY_MS : BE_LINK_OTA_RETRY_MS;
    const int max_tries = frame_op == BE_OTA_FINISH ? FINISH_TRIES : DATA_TRIES;
    if (now_ms - sent_ms >= retry) {
      if (tries >= max_tries) {
        portENTER_CRITICAL(&mux);
        fail(S32K_OTA_E_NO_ANSWER);
        portEXIT_CRITICAL(&mux);
        logging.println("S32K update: the S32K does not answer");
      } else {
        send_frame(now_ms);
      }
    }
    return;
  }

  if (phase == S32K_OTA_STARTING) {
    image_id = esp_random();
    be_link_ota_begin b = {};
    if (++frame_id == 0) {
      frame_id = 1;
    }
    b.hdr.op = BE_OTA_BEGIN;
    b.hdr.id = frame_id;
    b.hdr.image_id = image_id;
    b.hdr.total = st.total;
    memcpy(b.head, head, sizeof(b.head));  // sha256 stays zero: the S32K checks the image's own hash
    memcpy(frame, &b, sizeof(b));
    frame_len = sizeof(b);
    frame_op = BE_OTA_BEGIN;
    inflight = true;
    tries = 0;
    send_frame(now_ms);
    return;
  }

  if (phase == S32K_OTA_SENDING) {
    portENTER_CRITICAL(&mux);
    const uint32_t written = st.written, total = st.total;
    const size_t avail = ring_count;
    const bool finish = finish_requested;
    const uint32_t idle = millis() - last_piece_ms;
    size_t n = avail < BE_LINK_OTA_DATA_MAX ? avail : BE_LINK_OTA_DATA_MAX;
    // Whole frames of 1000 bytes (a multiple of 8), except the last one of the image
    if (n < BE_LINK_OTA_DATA_MAX && written + n != total) {
      n = 0;
    }
    if (n > 0) {
      size_t r = ring_read;
      for (size_t i = 0; i < n; i++) {
        frame[sizeof(be_link_ota_hdr) + i] = ring[r];
        r = (r + 1) % RING_SIZE;
      }
    }
    portEXIT_CRITICAL(&mux);

    if (n > 0) {
      // The header in front of the data copied above
      be_link_ota_hdr h = {};
      if (++frame_id == 0) {
        frame_id = 1;
      }
      h.op = BE_OTA_DATA;
      h.id = frame_id;
      h.image_id = image_id;
      h.total = total;
      h.offset = written;
      memcpy(frame, &h, sizeof(h));
      frame_len = (uint16_t)(sizeof(h) + n);
      frame_bytes = n;
      frame_op = BE_OTA_DATA;
      inflight = true;
      tries = 0;
      send_frame(now_ms);
    } else if (written == total && finish) {
      portENTER_CRITICAL(&mux);
      st.phase = S32K_OTA_CHECKING;
      portEXIT_CRITICAL(&mux);
      new_frame(BE_OTA_FINISH, 0, now_ms);
    } else if (avail == 0 && idle > STALL_MS) {
      send_abort();
      portENTER_CRITICAL(&mux);
      fail(S32K_OTA_E_STALLED);
      portEXIT_CRITICAL(&mux);
      logging.println("S32K update: the page stopped sending");
    }
  }
}

void s32k_ota_on_ack(const uint8_t* payload, uint16_t len) {
  be_link_ota_ack a = {};
  memcpy(&a, payload, len < sizeof(a) ? len : sizeof(a));
  if (!inflight || a.ack.acked_seq != frame_id || a.op != frame_op) {
    return;  // an ack of a frame repeated before
  }
  inflight = false;
  portENTER_CRITICAL(&mux);
  if (!phase_runs(st.phase)) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  if (a.ack.status != 0) {
    fail(a.ack.status);
    portEXIT_CRITICAL(&mux);
    logging.printf("S32K refused the update: %s (%d)\n", s32k_ota_error_text(a.ack.status), a.ack.status);
    return;
  }
  switch (a.op) {
    case BE_OTA_BEGIN:
      st.phase = S32K_OTA_SENDING;
      break;
    case BE_OTA_DATA:
      if (a.offset == st.written + frame_bytes) {
        ring_read = (ring_read + frame_bytes) % RING_SIZE;
        ring_count -= frame_bytes;
        st.written = a.offset;
      } else {
        fail(BE_OTA_E_SEQUENCE);
      }
      break;
    case BE_OTA_FINISH:
      st.phase = S32K_OTA_DONE;
      st.elapsed_ms = millis() - start_ms;
      done_ms = millis();
      break;
    default:
      break;
  }
  const bool done = st.phase == S32K_OTA_DONE;
  const uint32_t elapsed = st.elapsed_ms;
  portEXIT_CRITICAL(&mux);
  if (done) {
    logging.printf("S32K update %s checked and marked in %lu ms: the S32K restarts\n", st.version,
                   (unsigned long)elapsed);
  }
}

const char* s32k_ota_error_text(int e) {
  switch (e) {
    case 0:
      return "";
    case S32K_OTA_E_NO_LINK:
      return "no S32K on the link";
    case S32K_OTA_E_BUSY:
      return "an update is already running";
    case S32K_OTA_E_ESP32_FILE:
      return "this is an ESP32 firmware file, not an S32K one";
    case S32K_OTA_E_NO_ANSWER:
      return "the S32K stopped answering";
    case S32K_OTA_E_STALLED:
      return "the page stopped sending";
    case S32K_OTA_E_ABORTED:
      return "aborted";
    case S32K_OTA_E_MEMORY:
      return "not enough memory on the ESP32";
    case S32K_OTA_E_OFFSET:
      return "a piece out of order";
    case BE_OTA_E_NO_BOOTLOADER:
      return "the S32K runs without MCUboot: program it through SWD once";
    case BE_OTA_E_NOT_IMAGE:
      return "not an S32K image (zephyr.signed.bin)";
    case BE_OTA_E_TOO_BIG:
      return "the image is larger than the S32K's slot";
    case BE_OTA_E_FLAGS:
      return "the image has flags this board does not take";
    case BE_OTA_E_CONTACTORS:
      return "contactors closed or engaged: open them first";
    case BE_OTA_E_NOT_PAUSED:
      return "the emulator still allows charge or discharge: pause it first";
    case BE_OTA_E_CURRENT:
      return "current of 1 A or more";
    case BE_OTA_E_TEST_IMAGE:
      return "the S32K runs a new image that is not confirmed yet: wait a minute";
    case BE_OTA_E_SEQUENCE:
      return "transfer out of step";
    case BE_OTA_E_FLASH:
      return "writing the S32K's flash failed";
    case BE_OTA_E_HASH:
      return "the image is damaged (SHA-256 differs)";
    case BE_OTA_E_VECTORS:
      return "the image is not built for this slot";
    case BE_OTA_E_BOARD:
      return "the image is for another board";
    case BE_OTA_E_TLV:
      return "the image's TLV area is unreadable";
    case BE_OTA_E_MARK:
      return "marking the image for its test start failed";
    default:
      return "error";
  }
}

#endif  // CAN_VIA_LINK
