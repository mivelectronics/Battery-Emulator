#ifndef S32K_STORE_H
#define S32K_STORE_H

/*
 * Copies of S32K images on an ESP32 with 8 MB (s32k/OTA_PLAN.md section 5): the partitions s32k_a and s32k_b
 * (data, subtype 0x40, miv_8MB.csv) keep the images that went through this ESP32, the one the S32K runs now
 * ("current") and the one before ("previous"). The page can send a stored image again without the file;
 * it goes the same way as a file (s32k_ota.h). Nothing automatic: the way back after a failed start is
 * MCUboot's on the S32K. Without these partitions (4 MB table) nothing is stored.
 *
 * A slot: a 4 KB header of our own (struct below, CRC-32), the image from 4 KB on.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef CAN_VIA_LINK

enum s32k_store_state : uint32_t {
  S32K_STORE_EMPTY = 0,     // nothing (or a header that does not check)
  S32K_STORE_WRITTEN = 1,   // complete and its SHA-256 recorded, not taken by the S32K (refused or reverted)
  S32K_STORE_SENT = 2,      // the S32K checked and marked it: on test until it confirms
  S32K_STORE_CURRENT = 3,   // the S32K confirmed it: the image it runs
  S32K_STORE_PREVIOUS = 4,  // the current one before
};

struct s32k_store_info {
  uint8_t state;     // enum s32k_store_state
  uint32_t size;
  uint32_t counter;  // orders the slots: higher = newer
  char version[32];
};

bool s32k_store_available();
// Slot 0 = s32k_a, 1 = s32k_b. False if there is no such slot.
bool s32k_store_get(int slot, s32k_store_info* out);
const char* s32k_store_state_name(uint8_t state);

// From the transfer (s32k_ota.cpp): a new image goes into the slot that is not current
void s32k_store_begin(uint32_t total, const char* version);
void s32k_store_write(uint32_t offset, const uint8_t* data, size_t len);
void s32k_store_finish();  // all bytes in: SHA-256 and header, state WRITTEN
void s32k_store_sent();    // the S32K checked and marked it
void s32k_store_select(int slot);  // a stored image is being sent again: s32k_store_sent() marks that one
void s32k_store_cancel();  // the transfer ended without the S32K taking it
// The S32K confirmed version: its slot becomes current, the current one previous
void s32k_store_confirmed(const char* version);
// It did not confirm: MCUboot swapped it back. Its slot is kept, as not taken
void s32k_store_reverted(const char* version);

// Sending a stored image: checks its SHA-256 first. 0 or an S32K_OTA_E_* code.
int s32k_store_open(int slot, uint8_t head[32], uint32_t* size);
bool s32k_store_read(int slot, uint32_t offset, uint8_t* out, size_t len);

#endif  // CAN_VIA_LINK

#endif  // S32K_STORE_H
