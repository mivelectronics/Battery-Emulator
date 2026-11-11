/*
 * Test build only (CONFIG_MIV_BOOT_SLOW_SWAP, tools/build_ota.sh prodslow): makes a swap of MCUboot long and
 * visible, so that the power can be cut in the middle of it by hand (OTA_PLAN.md, step 4).
 *
 * - blue LED on while MCUboot runs, red LED blinking while it swaps (from MCUBOOT_STATUS_UPGRADING, for an
 *   update and for a swap back alike), both off when it jumps to the application (which blinks green)
 * - every watchdog feed during that swap (one per erased sector and per copied chunk) waits
 *   CONFIG_MIV_BOOT_SLOW_SWAP_US; the feeds and the time of the swap go to the console, to tune it
 * - a swap that MCUboot resumes after a power loss starts without MCUBOOT_STATUS_UPGRADING: it runs at full
 *   speed and the red LED stays dark, the console tells it apart
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <bootutil/mcuboot_status.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "miv_boot.h"

static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static bool swapping;
static uint32_t feeds;
static int64_t started_ms;

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status) {
	case MCUBOOT_STATUS_STARTUP:
		(void)gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
		(void)gpio_pin_configure_dt(&blue, GPIO_OUTPUT_ACTIVE);
		printk("MIV test: MCUboot runs (blue LED)\n");
		break;
	case MCUBOOT_STATUS_UPGRADING:
		swapping = true;
		feeds = 0;
		started_ms = k_uptime_get();
		printk("MIV test: swap starts, slowed down (red LED blinks)\n");
		break;
	case MCUBOOT_STATUS_BOOTABLE_IMAGE_FOUND:
		if (swapping) {
			printk("MIV test: swap done, %u steps, %u ms\n", feeds,
			       (unsigned int)(k_uptime_get() - started_ms));
		}
		swapping = false;
		(void)gpio_pin_set_dt(&red, 0);
		(void)gpio_pin_set_dt(&blue, 0);
		break;
	default:
		printk("MIV test: MCUboot status %d\n", (int)status);
		break;
	}
}

void miv_boot_slow_swap_step(void)
{
	if (!swapping) {
		return;
	}
	feeds++;
	k_busy_wait(CONFIG_MIV_BOOT_SLOW_SWAP_US);
	if ((feeds % 32U) == 0U) {
		(void)gpio_pin_toggle_dt(&red);
	}
}
