/*
 * S32K emulator firmware (MIV variant A+). The S32K runs on its own: CAN channels, settings in
 * NVS, Modbus RTU server for SCADA/HA. The ESP32 network module is optional and talks over the
 * link UART (today: CAN bridge for the unmodified Battery-Emulator on the ESP32).
 *
 * Next steps (FIRMWARE_PLAN.md): Battery-Emulator drivers and datalayer on this side, datalayer
 * and settings exchange with the ESP32.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include <string.h>

#include "be_cfg.h"
#include "be_core.h"
#include "be_kv.h"
#include "be_link_cmd.h"
#include "be_ota.h"
#include "be_wdt.h"
#include "can_io.h"
#include "link.h"
#include "modbus_srv.h"
#include "sim_battery.h"

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void on_link_frame(const struct be_link_frame *f)
{
	if (be_link_cmd_frame(f)) {
		return;
	}
	/* CMD from the ESP32 (bridge mode): set the bit rate of a channel, stored in NVS */
	if (f->type == BE_LINK_CMD) {
		struct be_link_cmd c = {0};

		memcpy(&c, f->payload, MIN(f->len, sizeof(c)));
		if (c.code == 1 && be_core_active()) {
			/* The drivers set the bit rates. The ESP32 asks at start from its own settings */
			struct be_link_ack a = {.acked_type = f->type, .acked_seq = f->seq, .status = -EPERM};

			link_send(BE_LINK_ACK, &a, sizeof(a));
		} else if (c.code == 1) {
			uint8_t iface = c.arg0 & 0xFF;
			struct be_cfg next = be_cfg;
			int err = -EINVAL;

			if (iface < BE_NUM_CAN) {
				next.can[iface].nominal_kbps = (uint16_t)(c.arg0 >> 8);
				next.can[iface].data_kbps = (uint16_t)c.arg1;
				next.can[iface].enabled = 1;
				err = 0;
				if (memcmp(&next, &be_cfg, sizeof(next)) != 0) {
					err = be_cfg_save(&next);
				}
				if (err == 0) {
					err = can_io_apply(iface);
				}
			}
			struct be_link_ack a = {.acked_type = f->type, .acked_seq = f->seq,
						.status = (int16_t)err};

			link_send(BE_LINK_ACK, &a, sizeof(a));
		} else if (c.code == 2 && !be_core_active()) {
			/* Diagnostic: internal loopback, arg0 = iface, arg1 = on/off */
			int err = can_io_set_loopback((uint8_t)c.arg0, c.arg1 != 0);

			printk("CAN%u loopback %u: %d\n", (unsigned int)c.arg0, (unsigned int)c.arg1, err);
		}
	}
}

static void core_can_rx(uint8_t iface, const struct can_frame *frame)
{
	be_core_rx_isr(iface, frame);
}

int main(void)
{
	/*
	 * Reset escalation off (threshold 0): with the default of 15 the chip counts its resets, and a
	 * device that restarts itself now and then must not stop doing so after some number of them.
	 */
	MC_RGM->DRET = 0;
	MC_RGM->FRET = 0;

	printk("\n=== BE S32K emulator (A+) on %s ===\n", CONFIG_BOARD_TARGET);
	be_wdt_boot();

	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	be_ota_init(be_cfg_init());
	/* With a battery type set the core owns the CAN channels, otherwise they go to the link */
	can_io_init(be_cfg.battery_type != 0 ? core_can_rx : link_queue_can_rx);
	if (link_init(on_link_frame)) {
		printk("link UART not ready\n");
	}
	modbus_srv_init();
	be_core_start();
	be_wdt_start();

	int64_t next_led = k_uptime_get(), next_stats = k_uptime_get() + 5000;

	while (1) {
		link_poll(K_MSEC(50));
		be_link_cmd_poll();
		be_kv_store_pending();
		be_wdt_main_alive();
		be_ota_poll();
#ifdef BE_SIM
		sim_battery_main_poll();
#endif

		int64_t now = k_uptime_get();

		if (now >= next_led) {
			next_led += 500;
			if (gpio_is_ready_dt(&led)) {
				gpio_pin_toggle_dt(&led);
			}
		}
		if (now >= next_stats) {
			next_stats += 10000;
			if (be_core_active()) {
				be_core_print_status();
			}
			const struct be_link_stats *ls = link_rx_stats();

			printk("cfg rev %u | link tx %u rx %u B ok %u crc %u frm %u ovf %u | uart or %u fe %u nf %u "
			       "drop %u | peer %d | can rx %u/%u/%u tx %u/%u/%u err %u/%u/%u\n",
			       be_cfg.revision, link_stats.tx_frames, link_stats.rx_bytes,
			       ls->frames_ok, ls->crc_errors, ls->framing_errors, ls->overflows,
			       link_stats.uart_overrun, link_stats.uart_framing, link_stats.uart_noise,
			       link_stats.rx_drops, link_peer_alive(), can_io_stats[0].rx,
			       can_io_stats[1].rx, can_io_stats[2].rx, can_io_stats[0].tx_ok,
			       can_io_stats[1].tx_ok, can_io_stats[2].tx_ok, can_io_stats[0].tx_err,
			       can_io_stats[1].tx_err, can_io_stats[2].tx_err);
		}
	}
	return 0;
}
