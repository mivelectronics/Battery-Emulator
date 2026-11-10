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
  BE_LINK_OTA = 9,      /* update of the CAN processor's image, struct be_link_ota_* below (NET -> CAN) */
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

/*
 * Update of the CAN processor's application (BE_LINK_OTA, NET -> CAN). The image is a signed MCUboot
 * image (zephyr.signed.bin). One frame in flight: the sender waits for the ack of a frame (or repeats it
 * after BE_LINK_OTA_RETRY_MS) before the next. While an update runs, the CAN processor stops its core,
 * sends no snapshots or CAN frames and refuses other messages; the network side holds them back.
 *
 *   BEGIN  struct be_link_ota_begin: size and MCUboot header of the image, optionally the SHA-256 of the
 *          whole file. Checks the header and the safety conditions, stops the core.
 *   DATA   struct be_link_ota_hdr + bytes at hdr.offset, in order, at most BE_LINK_OTA_DATA_MAX, a
 *          multiple of 8 except the last frame. Written to slot 1 (sectors erased as the data reaches them)
 *          and read back before the ack.
 *   FINISH struct be_link_ota_hdr: checks the image in slot 1 (its own SHA-256 TLV as MCUboot checks it,
 *          the file's SHA-256 if BEGIN had one, vector table, board id), marks it
 *          for a test start and restarts 2 s after the ack. MCUboot swaps it in; it confirms itself or
 *          MCUboot swaps the old image back.
 *   ABORT  struct be_link_ota_hdr: nothing is marked, restart into the running image.
 * A transfer that stops for 10 s is aborted the same way. Every frame gets a struct be_link_ota_ack.
 */
#define BE_LINK_OTA_DATA_MAX 1000u
#define BE_LINK_OTA_RETRY_MS 200u
#define BE_LINK_OTA_TIMEOUT_MS 10000u

enum be_link_ota_op {
  BE_OTA_BEGIN = 1,
  BE_OTA_DATA = 2,
  BE_OTA_FINISH = 3,
  BE_OTA_ABORT = 4,
};

struct be_link_ota_hdr {
  uint8_t op;        /* enum be_link_ota_op */
  uint8_t id;        /* chosen by the sender per frame, returned as acked_seq */
  uint16_t reserved;
  uint32_t image_id; /* chosen by the sender per transfer */
  uint32_t total;    /* image size in bytes */
  uint32_t offset;   /* DATA: offset of the bytes that follow */
} BE_LINK_PACKED;

struct be_link_ota_begin {
  struct be_link_ota_hdr hdr;
  uint8_t sha256[32]; /* of the whole file; all zero: not given (a browser on http cannot hash) */
  uint8_t head[32];   /* the first 32 bytes of the image: the MCUboot header */
} BE_LINK_PACKED;

struct be_link_ota_ack {
  struct be_link_ack ack; /* acked_type BE_LINK_OTA, acked_seq = hdr.id, status 0 or BE_OTA_E_* */
  uint8_t op;             /* the op acknowledged */
  uint8_t reserved[3];
  uint32_t offset; /* bytes of the image written so far: where the next DATA starts */
} BE_LINK_PACKED;

/* Status of an OTA ack. The page shows a text for each. */
#define BE_OTA_E_NO_BOOTLOADER (-200) /* the running image was not started by MCUboot */
#define BE_OTA_E_NOT_IMAGE (-201)     /* no MCUboot image (an ESP32 file starts with 0xE9) */
#define BE_OTA_E_TOO_BIG (-202)       /* larger than slot 1 */
#define BE_OTA_E_FLAGS (-203)         /* encrypted, RAM load or other flags this board does not take */
#define BE_OTA_E_CONTACTORS (-204)    /* a contactor output is on or the contactors are engaged */
#define BE_OTA_E_NOT_PAUSED (-205)    /* charge or discharge power allowed: pause the emulator first */
#define BE_OTA_E_CURRENT (-206)       /* current of 1 A or more */
#define BE_OTA_E_TEST_IMAGE (-207)    /* the running image is on test, not confirmed: slot 1 is the way back */
#define BE_OTA_E_SEQUENCE (-208)      /* no BEGIN, another transfer, or not the next offset (ack.offset is) */
#define BE_OTA_E_FLASH (-209)         /* erase, write or read back failed */
#define BE_OTA_E_HASH (-210)          /* SHA-256 differs (the image's own, or the file's from BEGIN) */
#define BE_OTA_E_VECTORS (-211)       /* vector table does not point into slot 0 and RAM */
#define BE_OTA_E_BOARD (-212)         /* board id (TLV 0xA0) missing or for another board */
#define BE_OTA_E_TLV (-213)           /* TLV area of the image unreadable */
#define BE_OTA_E_MARK (-214)          /* marking the image for the test start failed */

/* TLV in the protected area of the image with the board's name (imgtool --custom-tlv 0xA0 <name>) */
#define BE_OTA_TLV_BOARD 0xA0u

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
