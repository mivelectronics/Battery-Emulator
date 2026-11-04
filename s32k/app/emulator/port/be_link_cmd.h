/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_LINK_CMD_H
#define BE_LINK_CMD_H

#include <stdbool.h>
#include <stdint.h>

#include "be_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* From the link: true if the frame was a command or settings frame of this module, acknowledged. */
bool be_link_cmd_frame(const struct be_link_frame *frame);

/* From the main loop: commits settings and sends the sync. */
void be_link_cmd_poll(void);
void be_link_cmd_request_sync(void);

/* For the core thread */
bool be_link_cmd_pop_core(struct be_link_cmd *cmd);
uint16_t be_link_cmd_pop_live(void *out, uint16_t cap);

uint32_t be_settings_revision(void);

#ifdef __cplusplus
}
#endif

#endif /* BE_LINK_CMD_H */
