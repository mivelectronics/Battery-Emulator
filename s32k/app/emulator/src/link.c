/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "link.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "can_io.h"

#define FW_VERSION "s32k-emulator 0.2"

static const struct device *const link_uart = DEVICE_DT_GET(DT_ALIAS(be_link));

RING_BUF_DECLARE(link_tx_rb, 8192);
RING_BUF_DECLARE(link_rx_rb, 4096);
static K_MUTEX_DEFINE(link_tx_lock);
static K_SEM_DEFINE(link_wake, 0, 1);
K_MSGQ_DEFINE(can_rx_q, sizeof(struct be_link_can), 64, 4);

struct link_stats link_stats;
static struct be_link_rx rx;
static link_frame_fn frame_handler;
static uint8_t tx_seq;
static int64_t last_peer_hello = -100000;
static int64_t next_hello;

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

				link_stats.rx_bytes += n;
				link_stats.rx_drops += n - put;
				k_sem_give(&link_wake);
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

int link_send(uint8_t type, const void *payload, uint16_t len)
{
	static uint8_t enc[BE_LINK_MAX_ENCODED];
	int ret = 0;

	k_mutex_lock(&link_tx_lock, K_FOREVER);
	size_t n = be_link_encode(type, tx_seq++, payload, len, enc, sizeof(enc));

	if (n == 0 || ring_buf_space_get(&link_tx_rb) < n) {
		link_stats.tx_drops++;
		ret = -ENOSPC;
	} else {
		ring_buf_put(&link_tx_rb, enc, n);
		link_stats.tx_frames++;
		uart_irq_tx_enable(link_uart);
	}
	k_mutex_unlock(&link_tx_lock);
	return ret;
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

void link_queue_can_rx(uint8_t iface, const struct can_frame *frame)
{
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
	if (k_msgq_put(&can_rx_q, &m, K_NO_WAIT) != 0) {
		link_stats.can_q_drops++;
	}
	k_sem_give(&link_wake);
}

static void handle_can(const struct be_link_frame *f)
{
	struct be_link_can m = {0};

	memcpy(&m, f->payload, MIN(f->len, sizeof(m)));
	if (m.len > 64) {
		return;
	}
	struct can_frame cf = {
		.id = m.id,
		.dlc = can_bytes_to_dlc(m.len),
		.flags = ((m.flags & BE_LINK_CAN_EXT) ? CAN_FRAME_IDE : 0) |
			 ((m.flags & BE_LINK_CAN_FD) ? CAN_FRAME_FDF : 0) |
			 ((m.flags & BE_LINK_CAN_BRS) ? CAN_FRAME_BRS : 0),
	};

	memcpy(cf.data, m.data, m.len);
	(void)can_io_send(m.iface, &cf);
}

int link_init(link_frame_fn on_frame)
{
	if (!device_is_ready(link_uart)) {
		return -ENODEV;
	}
	frame_handler = on_frame;
	be_link_rx_init(&rx);
	uart_irq_callback_user_data_set(link_uart, link_isr, NULL);
	uart_irq_rx_enable(link_uart);
	next_hello = k_uptime_get();
	return 0;
}

void link_poll(k_timeout_t timeout)
{
	k_sem_take(&link_wake, timeout);

	struct be_link_can m;

	while (k_msgq_get(&can_rx_q, &m, K_NO_WAIT) == 0) {
		link_send(BE_LINK_CAN, &m, be_link_can_wire_size(m.len));
	}

	uint8_t buf[64];
	uint32_t n;
	struct be_link_frame f;

	while ((n = ring_buf_get(&link_rx_rb, buf, sizeof(buf))) > 0) {
		for (uint32_t i = 0; i < n; i++) {
			if (!be_link_rx_byte(&rx, buf[i], &f)) {
				continue;
			}
			switch (f.type) {
			case BE_LINK_HELLO:
				if (k_uptime_get() - last_peer_hello > 3000) {
					send_hello(); /* peer (re)started: answer at once */
				}
				last_peer_hello = k_uptime_get();
				break;
			case BE_LINK_CAN:
				handle_can(&f);
				break;
			default:
				if (frame_handler) {
					frame_handler(&f);
				}
				break;
			}
		}
	}

	if (k_uptime_get() >= next_hello) {
		next_hello += 1000;
		send_hello();
	}
}

bool link_peer_alive(void)
{
	return k_uptime_get() - last_peer_hello < 3000;
}

const struct be_link_stats *link_rx_stats(void)
{
	return &rx.stats;
}
