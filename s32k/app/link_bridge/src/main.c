/*
 * S32K CAN processor, bridge mode (variant "B" of the MIV design).
 *
 * Forwards every frame received on the three CAN channels to the ESP32 over the link UART
 * (be_link protocol, LPUART4, 2 Mbit/s) and transmits the frames the ESP32 sends. The ESP32
 * keeps running unmodified Battery-Emulator logic; this proves the link, CAN FD and timing
 * before the emulator itself moves to the S32K (variant "A").
 *
 * Link messages handled here:
 *   CAN  (NET -> CAN, flag TX): transmit on iface 0..2
 *   CMD  code 1: set bitrate, arg0 = iface | nominal << 8 (kbit/s), arg1 = data bitrate (kbit/s,
 *        0 = classic CAN only); restarts that channel
 *   HELLO: answered with our own HELLO
 * Sent from here: HELLO every second, CAN for every received frame, LOG for status text.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

#include "be_link.h"

#define FW_VERSION "s32k-bridge 0.1"
#define NUM_CAN    3

static const struct device *const link_uart = DEVICE_DT_GET(DT_ALIAS(be_link));
static const struct device *const cans[NUM_CAN] = {
	DEVICE_DT_GET(DT_NODELABEL(flexcan_0)), /* iface 0: on-board TJA1043, J21 */
	DEVICE_DT_GET(DT_NODELABEL(flexcan_1)), /* iface 1: header PTB22/23 */
	DEVICE_DT_GET(DT_NODELABEL(flexcan_4)), /* iface 2: header PTB3/PTB2 */
};
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* ---- link UART, interrupt driven ---------------------------------------------------------- */

RING_BUF_DECLARE(link_tx_rb, 8192);
RING_BUF_DECLARE(link_rx_rb, 4096);
static K_MUTEX_DEFINE(link_tx_lock);
static K_SEM_DEFINE(link_rx_sem, 0, 1);

static struct {
	uint32_t tx_frames, tx_drops, rx_bytes, rx_drops;
	uint32_t can_rx[NUM_CAN], can_tx_ok[NUM_CAN], can_tx_err[NUM_CAN], can_rx_drops;
} st;

static void link_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);
	while (uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[32];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				uint32_t put = ring_buf_put(&link_rx_rb, buf, n);

				st.rx_bytes += n;
				st.rx_drops += n - put;
				k_sem_give(&link_rx_sem);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *p;
			uint32_t n = ring_buf_get_ptr(&link_tx_rb, &p, 0);

			if (n == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int sent = uart_fifo_fill(dev, p, MIN(n, 64U));

				ring_buf_consume(&link_tx_rb, sent > 0 ? sent : 0);
			}
		}
	}
}

static uint8_t link_seq;

static int link_send(uint8_t type, const void *payload, uint16_t len)
{
	static uint8_t enc[BE_LINK_MAX_ENCODED];
	int ret = 0;

	k_mutex_lock(&link_tx_lock, K_FOREVER);
	size_t n = be_link_encode(type, link_seq++, payload, len, enc, sizeof(enc));

	if (n == 0 || ring_buf_space_get(&link_tx_rb) < n) {
		st.tx_drops++;
		ret = -ENOSPC;
	} else {
		ring_buf_put(&link_tx_rb, enc, n);
		st.tx_frames++;
		uart_irq_tx_enable(link_uart);
	}
	k_mutex_unlock(&link_tx_lock);
	return ret;
}

static void link_log(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n > 0) {
		printk("%s\n", buf);
		link_send(BE_LINK_LOG, buf, (uint16_t)MIN(n, (int)sizeof(buf) - 1));
	}
}

static void send_hello(void)
{
	struct be_link_hello h = {
		.proto_version = BE_LINK_PROTO_VERSION,
		.role = BE_LINK_ROLE_CAN,
	};

	strncpy(h.fw_version, FW_VERSION, sizeof(h.fw_version) - 1);
	link_send(BE_LINK_HELLO, &h, sizeof(h));
}

/* ---- CAN ------------------------------------------------------------------------------------ */

K_MSGQ_DEFINE(can_rx_q, sizeof(struct be_link_can), 64, 4);

static void can_rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	uint8_t iface = (uint8_t)(uintptr_t)user_data;
	struct be_link_can m = {
		.iface = iface,
		.flags = ((frame->flags & CAN_FRAME_IDE) ? BE_LINK_CAN_EXT : 0) |
			 ((frame->flags & CAN_FRAME_FDF) ? BE_LINK_CAN_FD : 0) |
			 ((frame->flags & CAN_FRAME_BRS) ? BE_LINK_CAN_BRS : 0),
		.len = can_dlc_to_bytes(frame->dlc),
		.id = frame->id,
		.timestamp_us = k_cyc_to_us_floor32(k_cycle_get_32()),
	};

	memcpy(m.data, frame->data, m.len);
	st.can_rx[iface]++;
	if (k_msgq_put(&can_rx_q, &m, K_NO_WAIT) != 0) {
		st.can_rx_drops++;
	}
	k_sem_give(&link_rx_sem); /* wake the link thread */
}

static void can_tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	uint8_t iface = (uint8_t)(uintptr_t)user_data;

	if (error == 0) {
		st.can_tx_ok[iface]++;
	} else {
		st.can_tx_err[iface]++;
	}
}

static int can_configure(uint8_t iface, uint32_t nominal, uint32_t data)
{
	const struct device *dev = cans[iface];
	struct can_timing t, td;
	int err;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	(void)can_stop(dev);
	err = can_calc_timing(dev, &t, nominal, 875);
	if (err < 0) {
		return err;
	}
	err = can_set_timing(dev, &t);
	if (err) {
		return err;
	}
	can_mode_t mode = CAN_MODE_NORMAL;

	if (data) {
		err = can_calc_timing_data(dev, &td, data, 750);
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
	return can_start(dev);
}

static void handle_frame(const struct be_link_frame *f)
{
	switch (f->type) {
	case BE_LINK_HELLO:
		send_hello();
		break;
	case BE_LINK_CAN: {
		struct be_link_can m = {0};

		memcpy(&m, f->payload, MIN(f->len, sizeof(m)));
		if (m.iface >= NUM_CAN || m.len > 64) {
			break;
		}
		struct can_frame cf = {
			.id = m.id,
			.dlc = can_bytes_to_dlc(m.len),
			.flags = ((m.flags & BE_LINK_CAN_EXT) ? CAN_FRAME_IDE : 0) |
				 ((m.flags & BE_LINK_CAN_FD) ? CAN_FRAME_FDF : 0) |
				 ((m.flags & BE_LINK_CAN_BRS) ? CAN_FRAME_BRS : 0),
		};
		memcpy(cf.data, m.data, m.len);
		/* Never block: with a NULL callback can_send() waits for an ACK */
		if (can_send(cans[m.iface], &cf, K_NO_WAIT, can_tx_cb, (void *)(uintptr_t)m.iface)) {
			st.can_tx_err[m.iface]++;
		}
		break;
	}
	case BE_LINK_CMD: {
		struct be_link_cmd c = {0};

		memcpy(&c, f->payload, MIN(f->len, sizeof(c)));
		if (c.code == 1) {
			uint8_t iface = c.arg0 & 0xFF;
			uint32_t nominal = (c.arg0 >> 8) * 1000U;
			uint32_t data = c.arg1 * 1000U;
			int err = iface < NUM_CAN ? can_configure(iface, nominal, data) : -EINVAL;
			struct be_link_ack a = {.acked_type = f->type, .acked_seq = f->seq, .status = (int16_t)err};

			link_send(BE_LINK_ACK, &a, sizeof(a));
			link_log("CAN%u set to %u/%u kbit/s: %d", iface, nominal / 1000, data / 1000, err);
		}
		break;
	}
	default:
		break;
	}
}

int main(void)
{
	printk("\n=== BE S32K %s on %s ===\n", FW_VERSION, CONFIG_BOARD_TARGET);

	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	if (!device_is_ready(link_uart)) {
		printk("link UART not ready\n");
		return 0;
	}
	uart_irq_callback_user_data_set(link_uart, link_isr, NULL);
	uart_irq_rx_enable(link_uart);

	const struct can_filter all_std = {.id = 0, .mask = 0, .flags = 0};
	const struct can_filter all_ext = {.id = 0, .mask = 0, .flags = CAN_FILTER_IDE};

	for (uint8_t i = 0; i < NUM_CAN; i++) {
		int err = can_configure(i, 500000, 2000000);

		if (err == 0) {
			can_add_rx_filter(cans[i], can_rx_cb, (void *)(uintptr_t)i, &all_std);
			can_add_rx_filter(cans[i], can_rx_cb, (void *)(uintptr_t)i, &all_ext);
		}
		printk("CAN iface %u (%s): %s\n", i, cans[i]->name, err ? "FAILED" : "500k/2M FD");
	}
	send_hello();

	struct be_link_rx rx;
	struct be_link_frame f;
	int64_t next_hello = k_uptime_get() + 1000, next_stats = k_uptime_get() + 5000;

	be_link_rx_init(&rx);
	while (1) {
		k_sem_take(&link_rx_sem, K_MSEC(50));

		struct be_link_can m;

		while (k_msgq_get(&can_rx_q, &m, K_NO_WAIT) == 0) {
			link_send(BE_LINK_CAN, &m, be_link_can_wire_size(m.len));
		}

		uint8_t buf[64];
		uint32_t n;

		while ((n = ring_buf_get(&link_rx_rb, buf, sizeof(buf))) > 0) {
			for (uint32_t i = 0; i < n; i++) {
				if (be_link_rx_byte(&rx, buf[i], &f)) {
					handle_frame(&f);
				}
			}
		}

		int64_t now = k_uptime_get();

		if (now >= next_hello) {
			next_hello += 1000;
			send_hello();
			if (gpio_is_ready_dt(&led)) {
				gpio_pin_toggle_dt(&led);
			}
		}
		if (now >= next_stats) {
			next_stats += 5000;
			printk("link: tx %u (drop %u) rx %u B (drop %u) ok %u crc %u frm %u | can rx %u/%u/%u "
			       "tx %u/%u/%u err %u/%u/%u qdrop %u\n",
			       st.tx_frames, st.tx_drops, st.rx_bytes, st.rx_drops, rx.stats.frames_ok,
			       rx.stats.crc_errors, rx.stats.framing_errors, st.can_rx[0], st.can_rx[1],
			       st.can_rx[2], st.can_tx_ok[0], st.can_tx_ok[1], st.can_tx_ok[2],
			       st.can_tx_err[0], st.can_tx_err[1], st.can_tx_err[2], st.can_rx_drops);
		}
	}
	return 0;
}
