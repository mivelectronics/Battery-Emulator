/*
 * Bench simulator: a Kona battery and a Pylon inverter inside the S32K, on the real CAN channels in
 * internal loopback. Only in builds with -DBE_SIM=1; never in the firmware for a real battery.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIM_BATTERY_H
#define SIM_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "be_core.h"

#ifdef BE_SIM
void sim_battery_start(void);
void sim_battery_tick(void);
/* Sees every frame; returns whether the drivers should get it. In loopback a driver would also hear
 * itself, which a real controller does not do, and the UDS layer takes its own requests for a
 * diagnostic tool on the bus and backs off. */
bool sim_battery_on_frame(const struct be_can_rx *frame);
int sim_battery_reg_read(uint16_t addr, uint16_t *value);
/* Register 400, written: what to break, see enum sim_command */
int sim_battery_command(uint16_t cmd);
void sim_battery_main_poll(void);
#else
static inline void sim_battery_start(void) {}
static inline void sim_battery_tick(void) {}
static inline bool sim_battery_on_frame(const struct be_can_rx *frame)
{
	(void)frame;
	return true;
}
#endif

#endif /* SIM_BATTERY_H */
