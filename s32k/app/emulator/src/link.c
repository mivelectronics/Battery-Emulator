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

/*
 * RX and TX run on DMA: the LPUART FIFO is only 4 bytes, 20 us at 2 Mbit/s, and with interrupts the
 * first bytes after an idle line were lost to overruns (measured on FRDM-A-S32K312). Buffers are
 * cache line aligned; the driver maintains the data cache around them.
 */
#define RX_BUF_SIZE 512
static uint8_t rx_bufs[2][RX_BUF_SIZE] __aligned(32);
static uint8_t rx_next;
static bool tx_busy;


static void start_tx(void)
{
	unsigned int key = irq_lock();

	if (!tx_busy) {
		uint8_t *p;
		uint32_t n = ring_buf_get_ptr(&link_tx_rb, &p, 0);

		if (n > 0) {
			tx_busy = true;
			if (uart_tx(link_uart, p, MIN(n, 512U), SYS_FOREVER_US) != 0) {
				tx_busy = false;
			}
		}
	}
	irq_unlock(key);
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		ring_buf_consume(&link_tx_rb, evt->data.tx.len);
		tx_busy = false;
		start_tx();
		break;
	case UART_RX_RDY: {
		uint32_t put = ring_buf_put(&link_rx_rb, evt->data.rx.buf + evt->data.rx.offset,
					    evt->data.rx.len);

		link_stats.rx_bytes += evt->data.rx.len;
		link_stats.rx_drops += evt->data.rx.len - put;
		k_sem_give(&link_wake);
		break;
	}
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(dev, rx_bufs[rx_next], RX_BUF_SIZE);
		rx_next ^= 1U;
		break;
	case UART_RX_STOPPED:
		link_stats.uart_overrun += (evt->data.rx_stop.reason & UART_ERROR_OVERRUN) ? 1 : 0;
		link_stats.uart_framing += (evt->data.rx_stop.reason & UART_ERROR_FRAMING) ? 1 : 0;
		link_stats.uart_noise += (evt->data.rx_stop.reason & UART_ERROR_NOISE) ? 1 : 0;
		break;
	case UART_RX_DISABLED:
		rx_next = 1U;
		uart_rx_enable(dev, rx_bufs[0], RX_BUF_SIZE, 100);
		break;
	default:
		break;
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
		start_tx();
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
	int err = uart_callback_set(link_uart, uart_cb, NULL);

	if (err) {
		return err;
	}
	rx_next = 1U;
	err = uart_rx_enable(link_uart, rx_bufs[0], RX_BUF_SIZE, 100);
	if (err) {
		return err;
	}
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
