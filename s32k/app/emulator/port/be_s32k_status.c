/*
 * The SNAP_S32K snapshot section: what only this chip knows about itself, for the pages on the ESP32.
 * The same figures as Modbus registers 10-13, 100-159 and 222-227.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <zephyr/kernel.h>

#include "be_core.h"
#include "be_gpio.h"
#include "be_snapshot.h"
#include "be_wdt.h"
#include "can_io.h"
#include "link.h"

BUILD_ASSERT(BE_S32K_NUM_CAN == BE_NUM_CAN, "SNAP_S32K carries every CAN channel");

void be_s32k_status_fill(struct be_s32k_status *s)
{
	memset(s, 0, sizeof(*s));
	s->reset_cause = (uint8_t)be_wdt_reset_cause();
	s->resets_in_row = (uint8_t)MIN(be_wdt_resets_in_row(), 0xFF);
	s->flags = (be_wdt_running() ? BE_S32K_F_WDT_RUNNING : 0) |
		   (be_wdt_contactors_locked_out() ? BE_S32K_F_LOCKOUT : 0) |
		   (be_core_active() ? BE_S32K_F_CORE : 0);
	s->outputs = be_gpio_levels();
	s->outputs_after_reset = be_gpio_outputs_after_reset();
	s->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	s->tick_max_us = be_core_tick_max_us;
	s->rx_drops = be_core_rx_drops;
	strncpy(s->fw_version, LINK_FW_VERSION, sizeof(s->fw_version) - 1);

	for (uint8_t i = 0; i < BE_NUM_CAN; i++) {
		enum can_state st = CAN_STATE_STOPPED;
		struct can_bus_err_cnt ec = {0};
		const struct can_io_stats *c = &can_io_stats[i];

		(void)can_io_state(i, &st, &ec);
		s->can[i].state = (uint8_t)st;
		s->can[i].tec = ec.tx_err_cnt;
		s->can[i].rec = ec.rx_err_cnt;
		s->can[i].nominal_kbps = c->nominal_kbps;
		s->can[i].data_kbps = c->data_kbps;
		s->can[i].rx = c->rx;
		s->can[i].tx_ok = c->tx_ok;
		s->can[i].tx_err = c->tx_err;
		s->can[i].tx_drops = c->tx_queue_drops;
	}
}
