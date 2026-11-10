#ifndef S32K_OTA_H
#define S32K_OTA_H

/*
 * Update of the S32K's application through this ESP32 (s32k/OTA_PLAN.md, section 4). The web page sends the
 * signed MCUboot image (zephyr.signed.bin) in pieces; the core task forwards it over the link as BE_LINK_OTA
 * frames, one in flight, repeated until acknowledged. While an update runs, the link is quiet: commands,
 * settings, live battery settings and CAN for the S32K are held back (only HELLO goes out).
 *
 * Web side (any task):   s32k_ota_begin / _chunk / _finish / _abort / _state
 * Core task:             s32k_ota_poll from the link poll, s32k_ota_on_ack for BE_LINK_OTA acks
 */

#include <stddef.h>
#include <stdint.h>

#ifdef CAN_VIA_LINK

enum s32k_ota_phase : uint8_t {
  S32K_OTA_IDLE = 0,
  S32K_OTA_STARTING = 1,  // BEGIN sent, waiting for the S32K's answer
  S32K_OTA_SENDING = 2,   // pieces flow: page -> buffer -> link
  S32K_OTA_CHECKING = 3,  // FINISH sent: the S32K checks the image
  S32K_OTA_DONE = 4,      // checked and marked: the S32K restarts, MCUboot swaps it in
  S32K_OTA_FAILED = 5,    // see error
};

// Errors of this side (the S32K's are BE_OTA_E_* in be_link.h)
#define S32K_OTA_E_NO_LINK (-100)     // no S32K on the link
#define S32K_OTA_E_BUSY (-101)        // an update runs already
#define S32K_OTA_E_ESP32_FILE (-102)  // the file is an ESP32 image: use the other field
#define S32K_OTA_E_NO_ANSWER (-103)   // the S32K stopped answering
#define S32K_OTA_E_STALLED (-104)     // the page stopped sending
#define S32K_OTA_E_ABORTED (-105)
#define S32K_OTA_E_MEMORY (-106)
#define S32K_OTA_E_OFFSET (-107)      // a piece that is not the next one
#define S32K_OTA_E_STORE (-108)       // the stored copy is empty or damaged

struct s32k_ota_state {
  uint8_t phase;      // enum s32k_ota_phase
  int16_t error;      // S32K_OTA_E_* or BE_OTA_E_*, 0 = none
  uint32_t total;     // image size
  uint32_t received;  // bytes taken from the page
  uint32_t written;   // bytes the S32K acknowledged as written
  uint32_t elapsed_ms;
  uint32_t repeats;   // frames sent again
  char version[32];   // from the MCUboot header, "255.255.65535+4294967295" at most
};

// Starts an update: head = the first 32 bytes of the file (MCUboot header), total = file size.
int s32k_ota_begin(const uint8_t* head, size_t head_len, uint32_t total);
// Adds piece data at offset (must be the next byte). -EAGAIN: buffer full, send it again shortly.
int s32k_ota_chunk(uint32_t offset, const uint8_t* data, size_t len);
// Free bytes in the buffer: a piece that does not fit is refused as a whole (-EAGAIN)
size_t s32k_ota_room();
// All pieces are in: the S32K checks and marks the image after the last one is written.
int s32k_ota_finish();
// Sends the image kept in store slot 0/1 (s32k_store.h) again, the same way as a file
int s32k_ota_install(int slot);
void s32k_ota_abort();
void s32k_ota_get(s32k_ota_state* out);
// True while the link has to stay quiet for an update
bool s32k_ota_active();
const char* s32k_ota_error_text(int error);

// Core task
void s32k_ota_poll(uint32_t now_ms);
void s32k_ota_on_ack(const uint8_t* payload, uint16_t len);

#endif  // CAN_VIA_LINK

#endif  // S32K_OTA_H
