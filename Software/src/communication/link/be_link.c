#include "be_link.h"

#include <string.h>

uint16_t be_link_crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)data[i] << 8);
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* COBS encode in -> out, no delimiter. Returns encoded length or 0 if out_cap is too small. */
static size_t cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
  size_t read = 0, write = 1, code_pos = 0;
  uint8_t code = 1;
  if (out_cap == 0) {
    return 0;
  }
  while (read < len) {
    if (write >= out_cap) {
      return 0;
    }
    if (in[read] == 0) {
      out[code_pos] = code;
      code = 1;
      code_pos = write++;
      read++;
    } else {
      out[write++] = in[read++];
      code++;
      if (code == 0xFF) {
        if (write >= out_cap) {
          return 0;
        }
        out[code_pos] = code;
        code = 1;
        code_pos = write++;
      }
    }
  }
  out[code_pos] = code;
  return write;
}

/* COBS decode in -> out (in has no delimiter). Returns decoded length or (size_t)-1 on error. */
static size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
  size_t read = 0, write = 0;
  while (read < len) {
    uint8_t code = in[read++];
    if (code == 0) {
      return (size_t)-1;
    }
    for (uint8_t i = 1; i < code; i++) {
      if (read >= len || write >= out_cap || in[read] == 0) {
        return (size_t)-1;
      }
      out[write++] = in[read++];
    }
    if (code != 0xFF && read < len) {
      if (write >= out_cap) {
        return (size_t)-1;
      }
      out[write++] = 0;
    }
  }
  return write;
}

size_t be_link_encode(uint8_t type, uint8_t seq, const void* payload, uint16_t len, uint8_t* out, size_t out_cap) {
  uint8_t raw[BE_LINK_MAX_RAW];
  if (len > BE_LINK_MAX_PAYLOAD || (len > 0 && payload == NULL)) {
    return 0;
  }
  raw[0] = type;
  raw[1] = seq;
  raw[2] = (uint8_t)(len & 0xFFu);
  raw[3] = (uint8_t)(len >> 8);
  if (len > 0) {
    memcpy(&raw[BE_LINK_HEADER_SIZE], payload, len);
  }
  size_t n = BE_LINK_HEADER_SIZE + len;
  uint16_t crc = be_link_crc16(raw, n);
  raw[n++] = (uint8_t)(crc & 0xFFu);
  raw[n++] = (uint8_t)(crc >> 8);

  if (out_cap < 3) {
    return 0;
  }
  out[0] = 0x00;
  size_t enc = cobs_encode(raw, n, out + 1, out_cap - 2);
  if (enc == 0) {
    return 0;
  }
  enc++;
  out[enc++] = 0x00;
  return enc;
}

void be_link_rx_init(struct be_link_rx* rx) {
  memset(rx, 0, sizeof(*rx));
}

int be_link_rx_byte(struct be_link_rx* rx, uint8_t byte, struct be_link_frame* frame) {
  if (byte != 0x00) {
    if (rx->n < sizeof(rx->enc)) {
      rx->enc[rx->n++] = byte;
    } else {
      rx->overflow = 1;
    }
    return 0;
  }

  /* End of frame */
  size_t n = rx->n;
  uint8_t overflow = rx->overflow;
  rx->n = 0;
  rx->overflow = 0;
  if (overflow) {
    rx->stats.overflows++;
    return 0;
  }
  if (n == 0) {
    return 0; /* back-to-back delimiters, e.g. a sender flushing the line */
  }
  size_t dec = cobs_decode(rx->enc, n, rx->dec, sizeof(rx->dec));
  if (dec == (size_t)-1 || dec < BE_LINK_HEADER_SIZE + BE_LINK_CRC_SIZE) {
    rx->stats.framing_errors++;
    return 0;
  }
  uint16_t len = (uint16_t)(rx->dec[2] | ((uint16_t)rx->dec[3] << 8));
  if (len > BE_LINK_MAX_PAYLOAD || dec != (size_t)BE_LINK_HEADER_SIZE + len + BE_LINK_CRC_SIZE) {
    rx->stats.framing_errors++;
    return 0;
  }
  uint16_t got = (uint16_t)(rx->dec[dec - 2] | ((uint16_t)rx->dec[dec - 1] << 8));
  if (be_link_crc16(rx->dec, dec - BE_LINK_CRC_SIZE) != got) {
    rx->stats.crc_errors++;
    return 0;
  }
  frame->type = rx->dec[0];
  frame->seq = rx->dec[1];
  frame->len = len;
  frame->payload = &rx->dec[BE_LINK_HEADER_SIZE];
  rx->stats.frames_ok++;
  return 1;
}
