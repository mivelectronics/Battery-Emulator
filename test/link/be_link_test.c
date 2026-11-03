/* Host test for the ESP32 <-> S32K link protocol. Build: see test/link/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Software/src/communication/link/be_link.h"

static int failures;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
      failures++;                                                   \
    }                                                               \
  } while (0)

static uint32_t rng = 12345;
static uint8_t rnd8(void) {
  rng = rng * 1103515245u + 12345u;
  return (uint8_t)(rng >> 16);
}

/* Feed a buffer, collect frames */
static int feed(struct be_link_rx* rx, const uint8_t* b, size_t n, struct be_link_frame* frames, uint8_t (*copies)[BE_LINK_MAX_PAYLOAD],
                int max) {
  int got = 0;
  for (size_t i = 0; i < n; i++) {
    struct be_link_frame f;
    if (be_link_rx_byte(rx, b[i], &f)) {
      if (got < max) {
        frames[got] = f;
        memcpy(copies[got], f.payload, f.len);
        frames[got].payload = copies[got];
      }
      got++;
    }
  }
  return got;
}

static uint8_t enc[4 * BE_LINK_MAX_ENCODED];
static uint8_t copies[8][BE_LINK_MAX_PAYLOAD];

static void test_crc(void) {
  /* CRC-16/CCITT-FALSE check value */
  CHECK(be_link_crc16((const uint8_t*)"123456789", 9) == 0x29B1);
}

static void test_roundtrip_sizes(void) {
  static const uint16_t sizes[] = {0, 1, 2, 253, 254, 255, 256, 508, 509, 1000, BE_LINK_MAX_PAYLOAD};
  for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    for (int pattern = 0; pattern < 3; pattern++) {
      uint8_t p[BE_LINK_MAX_PAYLOAD];
      for (uint16_t i = 0; i < sizes[k]; i++) {
        p[i] = pattern == 0 ? 0 : pattern == 1 ? 0xFF : rnd8();
      }
      size_t n = be_link_encode(BE_LINK_SNAPSHOT, (uint8_t)k, p, sizes[k], enc, sizeof(enc));
      CHECK(n > 0 && n <= BE_LINK_MAX_ENCODED);
      CHECK(enc[0] == 0 && enc[n - 1] == 0);
      for (size_t i = 1; i + 1 < n; i++) {
        CHECK(enc[i] != 0);
      }
      struct be_link_rx rx;
      be_link_rx_init(&rx);
      struct be_link_frame f[2];
      int got = feed(&rx, enc, n, f, copies, 2);
      CHECK(got == 1);
      CHECK(f[0].type == BE_LINK_SNAPSHOT && f[0].seq == (uint8_t)k && f[0].len == sizes[k]);
      CHECK(memcmp(f[0].payload, p, sizes[k]) == 0);
    }
  }
}

static void test_too_big(void) {
  uint8_t p[BE_LINK_MAX_PAYLOAD + 1] = {0};
  CHECK(be_link_encode(BE_LINK_LOG, 0, p, BE_LINK_MAX_PAYLOAD + 1, enc, sizeof(enc)) == 0);
  CHECK(be_link_encode(BE_LINK_LOG, 0, p, 10, enc, 5) == 0);
}

static void test_stream_and_resync(void) {
  /* garbage, frame A, garbage with zeros, frame B, frame C back to back */
  size_t pos = 0;
  for (int i = 0; i < 37; i++) {
    enc[pos++] = rnd8() | 1u;
  }
  struct be_link_cmd a = {.code = 7, .arg0 = 0xDEADBEEF, .arg1 = 0};
  pos += be_link_encode(BE_LINK_CMD, 1, &a, sizeof(a), enc + pos, sizeof(enc) - pos);
  enc[pos++] = 0x11;
  enc[pos++] = 0x00;
  enc[pos++] = 0x00;
  struct be_link_can c = {.iface = 2, .flags = BE_LINK_CAN_FD | BE_LINK_CAN_BRS, .len = 64, .id = 0x7E0};
  for (int i = 0; i < 64; i++) {
    c.data[i] = (uint8_t)i;
  }
  pos += be_link_encode(BE_LINK_CAN, 2, &c, be_link_can_wire_size(64), enc + pos, sizeof(enc) - pos);
  pos += be_link_encode(BE_LINK_LOG, 3, "hello", 5, enc + pos, sizeof(enc) - pos);

  struct be_link_rx rx;
  be_link_rx_init(&rx);
  struct be_link_frame f[8];
  int got = feed(&rx, enc, pos, f, copies, 8);
  CHECK(got == 3);
  CHECK(f[0].type == BE_LINK_CMD && f[0].len == sizeof(a));
  struct be_link_cmd a2;
  memcpy(&a2, f[0].payload, sizeof(a2));
  CHECK(a2.code == 7 && a2.arg0 == 0xDEADBEEF);
  CHECK(f[1].type == BE_LINK_CAN && f[1].len == be_link_can_wire_size(64));
  CHECK(f[1].payload[0] == 2 && f[1].payload[12 + 63] == 63);
  CHECK(f[2].type == BE_LINK_LOG && f[2].len == 5 && memcmp(f[2].payload, "hello", 5) == 0);
  CHECK(rx.stats.frames_ok == 3);
  CHECK(rx.stats.framing_errors + rx.stats.crc_errors >= 2); /* the two garbage chunks */
}

static void test_corruption(void) {
  /* Flip every byte of a frame in turn: never accepted as valid with wrong content */
  uint8_t p[300];
  for (int i = 0; i < 300; i++) {
    p[i] = rnd8();
  }
  size_t n = be_link_encode(BE_LINK_SETTINGS, 9, p, sizeof(p), enc, sizeof(enc));
  int accepted_bad = 0;
  for (size_t i = 1; i + 1 < n; i++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t save = enc[i];
      enc[i] ^= (uint8_t)(1u << bit);
      struct be_link_rx rx;
      be_link_rx_init(&rx);
      struct be_link_frame f[2];
      int got = feed(&rx, enc, n, f, copies, 2);
      for (int g = 0; g < got && g < 2; g++) {
        if (!(f[g].len == sizeof(p) && memcmp(f[g].payload, p, sizeof(p)) == 0)) {
          accepted_bad++;
        }
      }
      enc[i] = save;
    }
  }
  CHECK(accepted_bad == 0);
}

static void test_overflow(void) {
  struct be_link_rx rx;
  be_link_rx_init(&rx);
  struct be_link_frame f;
  for (size_t i = 0; i < BE_LINK_MAX_ENCODED + 50; i++) {
    be_link_rx_byte(&rx, 0x55, &f);
  }
  CHECK(be_link_rx_byte(&rx, 0x00, &f) == 0);
  CHECK(rx.stats.overflows == 1);
  /* still works afterwards */
  size_t n = be_link_encode(BE_LINK_ACK, 1, "\x01\x02\x00\x00", 4, enc, sizeof(enc));
  struct be_link_frame fr[1];
  CHECK(feed(&rx, enc, n, fr, copies, 1) == 1);
}

static void test_struct_sizes(void) {
  CHECK(sizeof(struct be_link_hello) == 36);
  CHECK(sizeof(struct be_link_chunk_hdr) == 12);
  CHECK(sizeof(struct be_link_can) == 76);
  CHECK(sizeof(struct be_link_cmd) == 12);
  CHECK(sizeof(struct be_link_event) == 8);
  CHECK(sizeof(struct be_link_ack) == 4);
}

int main(void) {
  test_crc();
  test_roundtrip_sizes();
  test_too_big();
  test_stream_and_resync();
  test_corruption();
  test_overflow();
  test_struct_sizes();
  if (failures) {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("be_link: all tests passed\n");
  return 0;
}
