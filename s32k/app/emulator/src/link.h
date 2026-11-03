/*
 * Link UART to the optional network module (ESP32): be_link protocol on LPUART4, 2 Mbit/s.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <zephyr/drivers/can.h>

#include "be_link.h"

struct link_stats {
	uint32_t tx_frames, tx_drops, rx_bytes, rx_drops, can_q_drops;
};
extern struct link_stats link_stats;

/* Handler for frames that are not CAN or HELLO (CMD, SETTINGS, ...). */
typedef void (*link_frame_fn)(const struct be_link_frame *frame);

int link_init(link_frame_fn on_frame);

/* Called from the CAN receive interrupt: queues the frame for the ESP32. */
void link_queue_can_rx(uint8_t iface, const struct can_frame *frame);

int link_send(uint8_t type, const void *payload, uint16_t len);

/* Runs the link: sends queued CAN frames, parses received bytes, HELLO once a second.
 * Call from the main loop; returns after at most timeout. */
void link_poll(k_timeout_t timeout);

/* True while HELLO frames keep arriving from the network module (last 3 s). */
bool link_peer_alive(void);

const struct be_link_stats *link_rx_stats(void);

#endif /* LINK_H */
