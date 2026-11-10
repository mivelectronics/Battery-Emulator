/*
 * The thread of the Battery-Emulator core: 1 ms tick like core_loop on the ESP32, and the queue of
 * received CAN frames. Cooperative priority: a tick is not interrupted by the Modbus or link threads,
 * so they never read the datalayer halfway through an update.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_core.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/printk.h>

#include "be_cfg.h"
#include "be_gpio.h"
#include "can_io.h"
#include "sim_battery.h"

#define CORE_STACK_SIZE 8192
#define CORE_PRIO       K_PRIO_COOP(2)

/* In DTCM: fast, and the 64 KB there are otherwise free. The CPU only: DMA stays out of it */
static char __dtcm_noinit_section __aligned(4) core_rx_buf[48 * sizeof(struct be_can_rx)];
static struct k_msgq core_rx_q;
Z_KERNEL_STACK_DEFINE_IN(core_stack, CORE_STACK_SIZE, __dtcm_noinit_section);
static struct k_thread core_thread_data;
static bool active;
static bool halted;
uint32_t be_core_rx_drops;
uint32_t be_core_tick_max_us;
volatile uint32_t be_core_ticks;

uint16_t be_core_stack_free(void)
{
	size_t unused = 0;

	(void)k_thread_stack_space_get(&core_thread_data, &unused);
	return (uint16_t)MIN(unused, 0xFFFF);
}

void be_core_rx_isr(uint8_t iface, const void *zephyr_can_frame)
{
	const struct can_frame *f = zephyr_can_frame;

	if (!active) {
		return; /* can_io is up before the core: nothing to deliver to yet */
	}
	struct be_can_rx m = {
		.id = f->id,
		.iface = iface,
		.ext = (f->flags & CAN_FRAME_IDE) ? 1 : 0,
		.fd = (f->flags & CAN_FRAME_FDF) ? 1 : 0,
		.len = can_dlc_to_bytes(f->dlc),
	};

	memcpy(m.data, f->data, m.len);
	if (k_msgq_put(&core_rx_q, &m, K_NO_WAIT) != 0) {
		be_core_rx_drops++;
	}
}

bool be_core_rx_pop(struct be_can_rx *frame)
{
	while (k_msgq_get(&core_rx_q, frame, K_NO_WAIT) == 0) {
		if (sim_battery_on_frame(frame)) {
			return true;
		}
	}
	return false;
}

static void core_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	be_core_setup();
	sim_battery_start();
	printk("core: running, battery type %u, inverter type %u\n", be_cfg.battery_type,
	       be_cfg.inverter_type);

	int64_t next = k_uptime_get();

	while (1) {
		uint32_t t0 = k_cycle_get_32();

		sim_battery_tick();
		be_core_tick();
		can_io_tx_pump();
		be_core_ticks++;

		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

		if (us > be_core_tick_max_us && k_uptime_get() > 3000) {
			be_core_tick_max_us = us; /* after start-up, which allocates and sets up channels */
		}
		next += 1;
		int64_t now = k_uptime_get();

		if (next < now) {
			next = now; /* a tick took longer than 1 ms: do not try to catch up */
		}
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

int be_core_start(void)
{
	if (be_cfg.battery_type == 0) {
		printk("core: off (battery type 0), CAN bridged to the link\n");
		return 0;
	}
	k_msgq_init(&core_rx_q, core_rx_buf, sizeof(struct be_can_rx), 48);
	active = true;
	k_thread_create(&core_thread_data, core_stack, K_KERNEL_STACK_SIZEOF(core_stack), core_thread,
			NULL, NULL, NULL, CORE_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&core_thread_data, "be_core");
	return 0;
}

bool be_core_active(void)
{
	return active;
}

bool be_core_mode(void)
{
	return active || halted;
}

void be_core_halt(void)
{
	if (active) {
		/* First not active: the watchdog then stops checking the ticks, received frames are dropped */
		active = false;
		halted = true;
		k_thread_suspend(&core_thread_data);
	}
	be_gpio_safe_off();
}
