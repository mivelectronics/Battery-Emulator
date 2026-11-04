/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "can_io.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>

static const struct device *const cans[BE_NUM_CAN] = {
	DEVICE_DT_GET(DT_NODELABEL(flexcan_0)), /* iface 0: on-board TJA1043, J21 */
	DEVICE_DT_GET(DT_NODELABEL(flexcan_1)), /* iface 1: header PTB22/23 */
	DEVICE_DT_GET(DT_NODELABEL(flexcan_4)), /* iface 2: header PTB3/PTB2 */
};

struct can_io_stats can_io_stats[BE_NUM_CAN];
static can_io_rx_fn rx_hook;
static bool loopback[BE_NUM_CAN]; /* diagnostic, not stored */

int can_io_set_loopback(uint8_t iface, bool on)
{
	if (iface >= BE_NUM_CAN) {
		return -EINVAL;
	}
	loopback[iface] = on;
	return can_io_apply(iface);
}

static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	uint8_t iface = (uint8_t)(uintptr_t)user_data;

	can_io_stats[iface].rx++;
	if (rx_hook) {
		rx_hook(iface, frame);
	}
}

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	uint8_t iface = (uint8_t)(uintptr_t)user_data;

	if (error == 0) {
		can_io_stats[iface].tx_ok++;
	} else {
		can_io_stats[iface].tx_err++;
	}
}

int can_io_apply(uint8_t iface)
{
	if (iface >= BE_NUM_CAN) {
		return -EINVAL;
	}
	const struct device *dev = cans[iface];
	const struct be_cfg_can *c = &be_cfg.can[iface];
	struct can_timing t, td;
	int err;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	(void)can_stop(dev);
	can_io_stats[iface].nominal_kbps = 0;
	can_io_stats[iface].data_kbps = 0;
	if (!c->enabled) {
		return 0;
	}
	err = can_calc_timing(dev, &t, c->nominal_kbps * 1000U, 875);
	if (err < 0) {
		return err;
	}
	err = can_set_timing(dev, &t);
	if (err) {
		return err;
	}
	can_mode_t mode = loopback[iface] ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;

	if (c->data_kbps) {
		err = can_calc_timing_data(dev, &td, c->data_kbps * 1000U, 750);
		if (err < 0) {
			return err;
		}
		err = can_set_timing_data(dev, &td);
		if (err) {
			return err;
		}
		mode |= CAN_MODE_FD;
	}
	err = can_set_mode(dev, mode);
	if (err) {
		return err;
	}
	err = can_start(dev);
	if (err == 0) {
		can_io_stats[iface].nominal_kbps = c->nominal_kbps;
		can_io_stats[iface].data_kbps = c->data_kbps;
	}
	return err;
}

int can_io_init(can_io_rx_fn rx)
{
	const struct can_filter all_std = {.id = 0, .mask = 0, .flags = 0};
	const struct can_filter all_ext = {.id = 0, .mask = 0, .flags = CAN_FILTER_IDE};
	int ret = 0;

	rx_hook = rx;
	for (uint8_t i = 0; i < BE_NUM_CAN; i++) {
		int err = can_io_apply(i);

		if (device_is_ready(cans[i])) {
			can_add_rx_filter(cans[i], rx_cb, (void *)(uintptr_t)i, &all_std);
			can_add_rx_filter(cans[i], rx_cb, (void *)(uintptr_t)i, &all_ext);
		}
		printk("CAN%u (%s): %u/%u kbit/s %s\n", i, cans[i]->name, be_cfg.can[i].nominal_kbps,
		       be_cfg.can[i].data_kbps, err ? "FAILED" : (be_cfg.can[i].enabled ? "on" : "off"));
		if (err) {
			ret = err;
		}
	}
	return ret;
}

int can_io_send(uint8_t iface, const struct can_frame *frame)
{
	if (iface >= BE_NUM_CAN) {
		return -EINVAL;
	}
	/* Never block: with a NULL callback can_send() waits for an ACK */
	int err = can_send(cans[iface], frame, K_NO_WAIT, tx_cb, (void *)(uintptr_t)iface);

	if (err) {
		can_io_stats[iface].tx_err++;
	}
	return err;
}

int can_io_state(uint8_t iface, enum can_state *state, struct can_bus_err_cnt *cnt)
{
	if (iface >= BE_NUM_CAN) {
		return -EINVAL;
	}
	return can_get_state(cans[iface], state, cnt);
}
