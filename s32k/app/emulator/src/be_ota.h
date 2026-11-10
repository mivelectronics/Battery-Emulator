/*
 * The image under MCUboot (OTA_PLAN.md): receiving an update over the link into slot 1, confirming a new
 * image, and the state for Modbus and the snapshot. Without MCUboot (a build straight into flash) the
 * image counts as confirmed and updates are refused.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_OTA_H
#define BE_OTA_H

#include <stdbool.h>
#include <stdint.h>

#include "be_link.h"

enum be_image_state {
	BE_IMAGE_NO_BOOTLOADER = 0, /* built without MCUboot */
	BE_IMAGE_CONFIRMED = 1,
	BE_IMAGE_TEST = 2, /* started by MCUboot as a test image, not confirmed yet */
};

enum be_update_result {
	BE_UPDATE_NONE = 0,
	BE_UPDATE_INSTALLED = 1, /* the update was confirmed in this start */
	BE_UPDATE_REVERTED = 2,  /* the update did not confirm: MCUboot swapped the old image back */
	BE_UPDATE_ON_TEST = 3,   /* this start is the update, not confirmed yet */
};

/* At start, after be_cfg_init(): its result decides whether the settings were read */
void be_ota_init(int cfg_err);

/*
 * From the main loop. Confirms a test image once: settings read, link to the ESP32 up, 60 s, and a start
 * that was not a watchdog or fault reset. A test image that is not confirmed 5 minutes after its start,
 * whatever the reason, restarts: MCUboot swaps the old image back. Also ends a stalled update transfer.
 */
void be_ota_poll(void);

/* From the link: true if the frame was BE_LINK_OTA (acknowledged here) */
bool be_ota_frame(const struct be_link_frame *frame);

/* An update is being received or was received and the restart is due: the link stays quiet */
bool be_ota_busy(void);

enum be_image_state be_ota_image_state(void);
enum be_update_result be_ota_update_result(void);
/* MCUboot version of the running image and of the update of be_ota_update_result(), "" if none */
const char *be_ota_image_version(void);
const char *be_ota_update_version(void);

#endif /* BE_OTA_H */
