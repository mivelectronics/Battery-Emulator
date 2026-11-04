#ifndef BE_LINK_H
#define BE_LINK_H

/*
 * Link between the network processor (ESP32: web UI, MQTT, OTA, settings) and the CAN processor
 * (NXP S32K3 under Zephyr: CAN, battery/inverter drivers, safety, contactors) of the MIV hardware.
 *
 * Plain C99, no allocation, shared byte for byte by both firmwares.
 *
 * Wire format of one frame:
 *     0x00  COBS( type u8 | seq u8 | len u16 LE | payload[len] | crc16 u16 LE )  0x00
 * COBS removes every zero byte, so 0x00 only ever delimits frames. Every frame starts with one too,
 * so noise or a half frame left on the line after a reset never swallows the next frame; the
 * receiver ignores the empty frame between two delimiters. CRC is CRC-16/CCITT-FALSE
 * (poly 0x1021, init 0xFFFF) over type..payload. All multi-byte fields are little endian.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BE_LINK_PROTO_VERSION 1u
#define BE_LINK_MAX_PAYLOAD 1024u
#define BE_LINK_HEADER_SIZE 4u
#define BE_LINK_CRC_SIZE 2u
#define BE_LINK_MAX_RAW (BE_LINK_HEADER_SIZE + BE_LINK_MAX_PAYLOAD + BE_LINK_CRC_SIZE)
/* COBS adds at most one byte per 254 plus one; plus the leading and trailing 0x00 */
#define BE_LINK_MAX_ENCODED (BE_LINK_MAX_RAW + BE_LINK_MAX_RAW / 254u + 3u)

enum be_link_type {
  BE_LINK_HELLO = 1,    /* struct be_link_hello, sent by both sides after start */
  BE_LINK_SNAPSHOT = 2, /* one section of the live state, see be_snapshot.cpp (CAN -> NET) */
  BE_LINK_SETTINGS = 3, /* struct be_link_chunk_hdr + part of the settings blob (NET -> CAN) */
  BE_LINK_CMD = 4,      /* struct be_link_cmd (NET -> CAN) */
  BE_LINK_CAN = 5,      /* struct be_link_can, raw CAN frame (bridge mode, replay, dump) */
  BE_LINK_EVENT = 6,    /* struct be_link_event (CAN -> NET) */
  BE_LINK_ACK = 7,      /* struct be_link_ack */
  BE_LINK_LOG = 8,      /* UTF-8 text, no terminator */
  BE_LINK_OTA = 9,      /* struct be_link_chunk_hdr + part of a CAN processor image (NET -> CAN) */
};

enum be_link_role {
  BE_LINK_ROLE_CAN = 1, /* S32K */
  BE_LINK_ROLE_NET = 2, /* ESP32 */
};

#if defined(__GNUC__) || defined(__clang__)
#define BE_LINK_PACKED __attribute__((packed))
#else
#define BE_LINK_PACKED
#endif

struct be_link_hello {
  uint16_t proto_version; /* BE_LINK_PROTO_VERSION */
  uint8_t role;           /* enum be_link_role */
  uint8_t reserved;
  uint32_t layout_hash; /* hash of the datalayer layout; both sides must match to use snapshots */
  uint32_t layout_size; /* sizeof() of the snapshot */
  char fw_version[24];  /* NUL padded */
} BE_LINK_PACKED;

/* Large objects (settings, OTA image) travel in chunks */
struct be_link_chunk_hdr {
  uint32_t object_id; /* layout hash for settings, image id for OTA */
  uint32_t total;     /* total object size in bytes */
  uint32_t offset;    /* offset of this chunk */
} BE_LINK_PACKED;

#define BE_LINK_CAN_EXT (1u << 0) /* 29-bit identifier */
#define BE_LINK_CAN_FD (1u << 1)
#define BE_LINK_CAN_BRS (1u << 2)
#define BE_LINK_CAN_TX (1u << 3) /* set: frame to transmit / was transmitted; clear: received */

struct be_link_can {
  uint8_t iface; /* CAN interface index on the CAN processor */
  uint8_t flags; /* BE_LINK_CAN_* */
  uint8_t len;   /* data length in bytes, 0..64 */
  uint8_t reserved;
  uint32_t id;
  uint32_t timestamp_us;
  uint8_t data[64]; /* only len bytes are sent */
} BE_LINK_PACKED;

struct be_link_cmd {
  uint16_t code;
  uint16_t reserved;
  uint32_t arg0;
  uint32_t arg1;
} BE_LINK_PACKED;

struct be_link_event {
  uint16_t event; /* EVENTS_ENUM_TYPE index */
  uint8_t level;
  uint8_t state; /* 1 set, 0 cleared */
  uint32_t data;
} BE_LINK_PACKED;

struct be_link_ack {
  uint8_t acked_type;
  uint8_t acked_seq;
  int16_t status; /* 0 ok, negative errno-style */
} BE_LINK_PACKED;

/* CRC-16/CCITT-FALSE */
uint16_t be_link_crc16(const uint8_t* data, size_t len);

/* Encode one frame into out (including the leading and trailing 0x00). Returns the number of bytes written,
 * or 0 if len > BE_LINK_MAX_PAYLOAD or out_cap is too small. */
size_t be_link_encode(uint8_t type, uint8_t seq, const void* payload, uint16_t len, uint8_t* out, size_t out_cap);

struct be_link_frame {
  uint8_t type;
  uint8_t seq;
  uint16_t len;
  const uint8_t* payload; /* points into the receiver's buffer, valid until the next byte is fed */
};

struct be_link_stats {
  uint32_t frames_ok;
  uint32_t crc_errors;
  uint32_t framing_errors; /* COBS errors, wrong length, too short */
  uint32_t overflows;      /* frame longer than BE_LINK_MAX_ENCODED */
};

struct be_link_rx {
  uint8_t enc[BE_LINK_MAX_ENCODED]; /* bytes of the current frame, COBS encoded */
  uint8_t dec[BE_LINK_MAX_RAW];
  size_t n;
  uint8_t overflow;
  struct be_link_stats stats;
};

void be_link_rx_init(struct be_link_rx* rx);

/* Feed one received byte. Returns 1 and fills *frame when the byte completed a valid frame,
 * 0 otherwise. Invalid frames are counted in rx->stats and dropped. */
int be_link_rx_byte(struct be_link_rx* rx, uint8_t byte, struct be_link_frame* frame);

/* Size of a CAN payload on the wire for a given data length (header + len bytes). */
static inline uint16_t be_link_can_wire_size(uint8_t len) {
  return (uint16_t)(sizeof(struct be_link_can) - 64u + (len > 64u ? 64u : len));
}

#ifdef __cplusplus
}
#endif

#endif /* BE_LINK_H */
