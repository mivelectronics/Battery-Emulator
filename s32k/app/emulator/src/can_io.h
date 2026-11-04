/*
 * The three CAN channels of the S32K: CAN0 (inverter, on-board transceiver), CAN1 and CAN4
 * (batteries, CAN FD).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CAN_IO_H
#define CAN_IO_H

#include <stdint.h>
#include <zephyr/drivers/can.h>

#include "be_cfg.h"

struct can_io_stats {
	uint32_t rx, tx_ok, tx_err, rx_drops;
	uint32_t tx_queue_drops; /* frames for can_io_send_raw that did not fit in the software queue */
	uint16_t nominal_kbps, data_kbps; /* what the channel actually runs at, 0 = stopped */
};

extern struct can_io_stats can_io_stats[BE_NUM_CAN];

/* Called for every received frame, from the CAN interrupt: keep it short. */
typedef void (*can_io_rx_fn)(uint8_t iface, const struct can_frame *frame);

/* Starts the channels from be_cfg and installs the receive hook. */
int can_io_init(can_io_rx_fn rx);

/* Applies be_cfg.can[iface] (restarts that channel). */
int can_io_apply(uint8_t iface);

/* Diagnostic: internal loopback on a channel (not stored, restarts the channel). */
int can_io_set_loopback(uint8_t iface, bool on);

/* Non-blocking transmit. */
int can_io_send(uint8_t iface, const struct can_frame *frame);

/*
 * Non-blocking transmit from plain values (for the C++ core, which must not see Zephyr headers).
 * Goes through a software queue: the drivers send bursts, and a FlexCAN with 64-byte mailboxes has only
 * a few of them. Only for the core thread, which also calls can_io_tx_pump() every tick.
 */
int can_io_send_raw(uint8_t iface, uint32_t id, bool ext, bool fd, uint8_t len, const uint8_t *data);
void can_io_tx_pump(void);

/* Runs a channel at this arbitration rate whatever be_cfg says: the driver on it knows best. */
int can_io_set_nominal(uint8_t iface, uint16_t kbps);

/* Stops all channels and refuses to send (true), or starts them again (false). */
void can_io_pause(bool paused);

/* State and error counters of a channel. */
int can_io_state(uint8_t iface, enum can_state *state, struct can_bus_err_cnt *cnt);

#endif /* CAN_IO_H */
