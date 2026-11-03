/*
 * FRDM-A-S32K312 bring-up for the Battery-Emulator CAN processor.
 *
 * 1. Prints the clock tree (core / AIPS) on the OpenSDA console.
 * 2. Blinks the green LED.
 * 3. CAN0 (TJA1043): internal loopback test with a classic and a CAN FD frame
 *    (500 kbit/s arbitration, 2 Mbit/s data), then switches to normal mode and
 *    sends a heartbeat frame every second (ID 0x7E0) and prints what it receives.
 * 4. Measures the jitter of a 10 ms k_timer, the period Battery-Emulator uses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/printk.h>
#include <string.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct device *const can0 = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static volatile uint32_t rx_count;
static volatile uint32_t rx_fd_count;

static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	rx_count++;
	if (frame->flags & CAN_FRAME_FDF) {
		rx_fd_count++;
	}
	printk("CAN0 RX id=0x%03x dlc=%u len=%u%s%s data0=%02x\n", frame->id, frame->dlc,
	       can_dlc_to_bytes(frame->dlc), (frame->flags & CAN_FRAME_FDF) ? " FD" : "",
	       (frame->flags & CAN_FRAME_BRS) ? " BRS" : "", frame->data[0]);
}

/* 10 ms timer jitter */
static uint32_t last_cyc;
static uint32_t min_us = UINT32_MAX, max_us, ticks;

static void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	uint32_t now = k_cycle_get_32();

	if (last_cyc != 0U) {
		uint32_t us = k_cyc_to_us_floor32(now - last_cyc);

		min_us = MIN(min_us, us);
		max_us = MAX(max_us, us);
	}
	last_cyc = now;
	ticks++;
}
K_TIMER_DEFINE(tick10, tick_fn, NULL);

static int can_setup(void)
{
	struct can_timing t, td;
	int err;

	if (!device_is_ready(can0)) {
		printk("CAN0 not ready\n");
		return -ENODEV;
	}

	uint32_t core_clk = 0;

	(void)can_get_core_clock(can0, &core_clk);
	printk("CAN0 core clock: %u Hz\n", core_clk);

	err = can_calc_timing(can0, &t, 500000, 875);
	if (err < 0) {
		printk("can_calc_timing failed %d\n", err);
		return err;
	}
	err = can_calc_timing_data(can0, &td, 2000000, 750);
	if (err < 0) {
		printk("can_calc_timing_data failed %d\n", err);
		return err;
	}
	err = can_set_timing(can0, &t);
	err |= can_set_timing_data(can0, &td);
	if (err) {
		printk("can_set_timing failed\n");
		return -EIO;
	}

	const struct can_filter all = {.id = 0, .mask = 0, .flags = 0};

	err = can_add_rx_filter(can0, rx_cb, NULL, &all);
	if (err < 0) {
		printk("can_add_rx_filter failed %d\n", err);
		return err;
	}
	return 0;
}

/* Raw pad input level from SIUL2 GPDI (byte per pad, needs input buffer enabled). */
static uint8_t siul2_in(unsigned int pad)
{
	return *(volatile uint8_t *)(0x40291500UL + pad) & 1U;
}

static volatile uint32_t tx_ok, tx_err;

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	if (error == 0) {
		tx_ok++;
	} else {
		tx_err++;
	}
}

static int send(uint32_t id, bool fd, uint8_t seq)
{
	struct can_frame f = {0};

	f.id = id;
	if (fd) {
		f.flags = CAN_FRAME_FDF | CAN_FRAME_BRS;
		f.dlc = can_bytes_to_dlc(64);
		for (int i = 0; i < 64; i++) {
			f.data[i] = (uint8_t)(seq + i);
		}
	} else {
		f.dlc = 8;
		for (int i = 0; i < 8; i++) {
			f.data[i] = (uint8_t)(seq + i);
		}
	}
	/* Non-blocking: with a NULL callback can_send() waits for the frame to be acknowledged,
	 * which never happens on a bus without another node. */
	return can_send(can0, &f, K_MSEC(100), tx_cb, NULL);
}

int main(void)
{
	printk("\n=== BE S32K bring-up on %s ===\n", CONFIG_BOARD_TARGET);
	printk("SystemCoreClock: %u Hz, sys clock: %u Hz\n", SystemCoreClock,
	       sys_clock_hw_cycles_per_sec());

	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	if (can_setup() == 0) {
		/* Phase 1: internal loopback, no bus needed */
		if (can_set_mode(can0, CAN_MODE_LOOPBACK | CAN_MODE_FD) == 0 && can_start(can0) == 0) {
			send(0x123, false, 1);
			send(0x456, true, 2);
			k_sleep(K_MSEC(50));
			printk("Loopback: rx=%u (fd=%u) -> %s\n", rx_count, rx_fd_count,
			       (rx_count == 2U && rx_fd_count == 1U) ? "PASS" : "FAIL");
			can_stop(can0);
		}
		/* Phase 2: normal mode on J21 */
		printk("Before normal mode: CAN0_RX(PTA6)=%u ERR_N(PTC23)=%u\n", siul2_in(6),
		       siul2_in(2 * 32 + 23));
		if (can_set_mode(can0, CAN_MODE_FD) == 0 && can_start(can0) == 0) {
			printk("CAN0 normal mode, 500k/2M FD, heartbeat 0x7E0 every 1 s\n");
		}
		for (int i = 0; i < 5; i++) {
			enum can_state st;
			struct can_bus_err_cnt ec;

			k_busy_wait(200000);
			can_get_state(can0, &st, &ec);
			printk("  diag %d: RX=%u ERR_N=%u state=%d tec=%u rec=%u ticks=%u\n", i,
			       siul2_in(6), siul2_in(2 * 32 + 23), st, ec.tx_err_cnt, ec.rx_err_cnt,
			       ticks);
		}
	}

	k_timer_start(&tick10, K_MSEC(10), K_MSEC(10));

	uint8_t seq = 0;

	while (1) {
		k_sleep(K_SECONDS(1));
		if (gpio_is_ready_dt(&led)) {
			gpio_pin_toggle_dt(&led);
		}
		int err = send(0x7E0, false, seq++);
		enum can_state st;
		struct can_bus_err_cnt ec;

		can_get_state(can0, &st, &ec);
		printk("t=%us 10ms timer: %u ticks, period min %u us max %u us | CAN0 state=%d tec=%u "
		       "rec=%u send=%d tx_ok=%u tx_err=%u rx=%u\n",
		       (uint32_t)(k_uptime_get() / 1000), ticks, min_us, max_us, st, ec.tx_err_cnt,
		       ec.rx_err_cnt, err, tx_ok, tx_err, rx_count);
		min_us = UINT32_MAX;
		max_us = 0;
	}
	return 0;
}
