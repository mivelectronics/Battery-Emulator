#include "s32k_store.h"

#ifdef CAN_VIA_LINK

#include <Arduino.h>
#include <esp_crc.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "../../devboard/utils/logging.h"
#include "s32k_ota.h"

static constexpr uint32_t HDR_SIZE = 4096;  // the image starts here
static constexpr uint32_t SECTOR = 4096;
static constexpr char MAGIC[8] = {'S', '3', '2', 'K', 'I', 'M', 'G', '1'};

struct StoreHdr {
  char magic[8];
  uint32_t hdr_version;  // 1
  uint32_t size;         // image bytes
  uint8_t sha256[32];    // of the image
  char version[32];      // from its MCUboot header
  uint32_t state;        // enum s32k_store_state
  uint32_t counter;      // higher = newer
  uint32_t crc32;        // of everything above
};
static_assert(sizeof(StoreHdr) <= HDR_SIZE, "header fits its sector");

static const esp_partition_t* parts[2];
static bool looked = false;

static const esp_partition_t* slot_part(int s) {
  if (!looked) {
    looked = true;
    parts[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "s32k_a");
    parts[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "s32k_b");
  }
  return (s == 0 || s == 1) ? parts[s] : nullptr;
}

bool s32k_store_available() {
  return slot_part(0) != nullptr && slot_part(1) != nullptr;
}

static uint32_t hdr_crc(const StoreHdr& h) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(StoreHdr, crc32));
}

static bool read_hdr(int s, StoreHdr* h) {
  const esp_partition_t* p = slot_part(s);
  if (p == nullptr || esp_partition_read(p, 0, h, sizeof(*h)) != ESP_OK) {
    return false;
  }
  return memcmp(h->magic, MAGIC, sizeof(MAGIC)) == 0 && h->hdr_version == 1 && h->crc32 == hdr_crc(*h) &&
         h->size > 0 && h->size <= p->size - HDR_SIZE;
}

static bool write_hdr(int s, StoreHdr* h) {
  const esp_partition_t* p = slot_part(s);
  h->crc32 = hdr_crc(*h);
  return p != nullptr && esp_partition_erase_range(p, 0, SECTOR) == ESP_OK &&
         esp_partition_write(p, 0, h, sizeof(*h)) == ESP_OK;
}

bool s32k_store_get(int s, s32k_store_info* out) {
  if (slot_part(s) == nullptr) {
    return false;
  }
  StoreHdr h;
  memset(out, 0, sizeof(*out));
  if (read_hdr(s, &h)) {
    out->state = (uint8_t)h.state;
    out->size = h.size;
    out->counter = h.counter;
    memcpy(out->version, h.version, sizeof(out->version));
    out->version[sizeof(out->version) - 1] = 0;
  }
  return true;
}

const char* s32k_store_state_name(uint8_t state) {
  switch (state) {
    case S32K_STORE_WRITTEN:
      return "not taken";
    case S32K_STORE_SENT:
      return "on test";
    case S32K_STORE_CURRENT:
      return "current";
    case S32K_STORE_PREVIOUS:
      return "previous";
    default:
      return "empty";
  }
}

/* ---- the slot being written or sent ---- */
static int w_slot = -1;
static bool w_ok = false;
static uint32_t w_total = 0, w_erased = 0;
static char w_version[32];
static mbedtls_sha256_context w_sha;

static uint32_t next_counter() {
  uint32_t c = 0;
  for (int s = 0; s < 2; s++) {
    StoreHdr h;
    if (read_hdr(s, &h) && h.counter > c) {
      c = h.counter;
    }
  }
  return c + 1;
}

void s32k_store_begin(uint32_t total, const char* version) {
  w_slot = -1;
  if (!s32k_store_available()) {
    return;
  }
  // The slot that is not current; of two others, the older one
  StoreHdr h[2];
  const bool valid[2] = {read_hdr(0, &h[0]), read_hdr(1, &h[1])};
  int s;
  if (valid[0] && h[0].state == S32K_STORE_CURRENT) {
    s = 1;
  } else if (valid[1] && h[1].state == S32K_STORE_CURRENT) {
    s = 0;
  } else if (!valid[0]) {
    s = 0;
  } else if (!valid[1]) {
    s = 1;
  } else {
    s = h[0].counter <= h[1].counter ? 0 : 1;
  }
  if (total > slot_part(s)->size - HDR_SIZE || esp_partition_erase_range(slot_part(s), 0, SECTOR) != ESP_OK) {
    logging.println("S32K store: the image is not kept on this ESP32");
    return;
  }
  w_slot = s;
  w_ok = true;
  w_total = total;
  w_erased = HDR_SIZE;
  strncpy(w_version, version, sizeof(w_version) - 1);
  w_version[sizeof(w_version) - 1] = 0;
  mbedtls_sha256_init(&w_sha);
  mbedtls_sha256_starts(&w_sha, 0);
}

void s32k_store_write(uint32_t offset, const uint8_t* data, size_t len) {
  if (w_slot < 0 || !w_ok) {
    return;
  }
  const esp_partition_t* p = slot_part(w_slot);
  const uint32_t at = HDR_SIZE + offset, end = at + len;
  while (w_ok && w_erased < end) {
    w_ok = esp_partition_erase_range(p, w_erased, SECTOR) == ESP_OK;
    w_erased += SECTOR;
  }
  w_ok = w_ok && esp_partition_write(p, at, data, len) == ESP_OK;
  if (w_ok) {
    mbedtls_sha256_update(&w_sha, data, len);
  } else {
    logging.println("S32K store: writing the copy failed, the update goes on without it");
  }
}

void s32k_store_finish() {
  if (w_slot < 0 || !w_ok) {
    return;
  }
  StoreHdr h = {};
  memcpy(h.magic, MAGIC, sizeof(MAGIC));
  h.hdr_version = 1;
  h.size = w_total;
  mbedtls_sha256_finish(&w_sha, h.sha256);
  mbedtls_sha256_free(&w_sha);
  memcpy(h.version, w_version, sizeof(h.version));
  h.state = S32K_STORE_WRITTEN;
  h.counter = next_counter();
  w_ok = write_hdr(w_slot, &h);
}

void s32k_store_select(int slot) {
  w_slot = slot;
  w_ok = true;
}

void s32k_store_sent() {
  StoreHdr h;
  if (w_slot >= 0 && w_ok && read_hdr(w_slot, &h)) {
    h.state = S32K_STORE_SENT;
    write_hdr(w_slot, &h);
  }
  w_slot = -1;
}

void s32k_store_cancel() {
  w_slot = -1;
}

static void set_state(int s, StoreHdr& h, uint32_t state) {
  if (h.state != state) {
    h.state = state;
    write_hdr(s, &h);
  }
}

void s32k_store_confirmed(const char* version) {
  StoreHdr h[2];
  const bool valid[2] = {read_hdr(0, &h[0]), read_hdr(1, &h[1])};
  int hit = -1;
  for (int s = 0; s < 2; s++) {
    if (valid[s] && h[s].state == S32K_STORE_SENT && strncmp(h[s].version, version, sizeof(h[s].version)) == 0) {
      hit = s;
    }
  }
  if (hit < 0) {
    return;
  }
  const int other = 1 - hit;
  if (valid[other] && h[other].state == S32K_STORE_CURRENT) {
    set_state(other, h[other], S32K_STORE_PREVIOUS);
  }
  set_state(hit, h[hit], S32K_STORE_CURRENT);
  logging.printf("S32K store: %s is the current image\n", version);
}

void s32k_store_reverted(const char* version) {
  for (int s = 0; s < 2; s++) {
    StoreHdr h;
    if (read_hdr(s, &h) && h.state == S32K_STORE_SENT && strncmp(h.version, version, sizeof(h.version)) == 0) {
      set_state(s, h, S32K_STORE_WRITTEN);
    }
  }
}

int s32k_store_open(int s, uint8_t head[32], uint32_t* size) {
  StoreHdr h;
  if (!read_hdr(s, &h) || h.state == S32K_STORE_EMPTY) {
    return S32K_OTA_E_STORE;
  }
  // The copy is read back whole before anything is sent
  const esp_partition_t* p = slot_part(s);
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t buf[4096];
  bool ok = true;
  for (uint32_t off = 0; ok && off < h.size; off += sizeof(buf)) {
    const uint32_t n = h.size - off < sizeof(buf) ? h.size - off : sizeof(buf);
    ok = esp_partition_read(p, HDR_SIZE + off, buf, n) == ESP_OK;
    mbedtls_sha256_update(&sha, buf, n);
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!ok || memcmp(digest, h.sha256, sizeof(digest)) != 0) {
    return S32K_OTA_E_STORE;
  }
  *size = h.size;
  return esp_partition_read(p, HDR_SIZE, head, 32) == ESP_OK ? 0 : S32K_OTA_E_STORE;
}

bool s32k_store_read(int s, uint32_t offset, uint8_t* out, size_t len) {
  const esp_partition_t* p = slot_part(s);
  return p != nullptr && esp_partition_read(p, HDR_SIZE + offset, out, len) == ESP_OK;
}

#endif  // CAN_VIA_LINK
