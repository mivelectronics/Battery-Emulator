/*
 * The image under MCUboot (OTA_PLAN.md): confirming a new image, and its state for Modbus and the
 * snapshot. Without MCUboot (a build straight into flash) the image counts as confirmed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_OTA_H
#define BE_OTA_H

#include <stdbool.h>
#include <stdint.h>

enum be_image_state {
	BE_IMAGE_NO_BOOTLOADER = 0, /* built without MCUboot */
	BE_IMAGE_CONFIRMED = 1,
	BE_IMAGE_TEST = 2, /* started by MCUboot as a test image, not confirmed yet */
};

/* At start, after be_cfg_init(): its result decides whether the settings were read */
void be_ota_init(int cfg_err);

/* From the main loop. Confirms a test image once: settings read, link to the ESP32 up, 60 s without a
 * watchdog or fault reset. Without the link for 5 minutes it restarts unconfirmed: MCUboot swaps the
 * old image back. */
void be_ota_poll(void);

enum be_image_state be_ota_image_state(void);

#endif /* BE_OTA_H */
