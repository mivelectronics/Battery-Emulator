/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_ota.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "be_cfg.h"
#include "be_wdt.h"
#include "link.h"

#ifdef CONFIG_BOOTLOADER_MCUBOOT
#include <zephyr/dfu/mcuboot.h>

#define CONFIRM_AFTER_MS (60U * 1000U)
#define NO_LINK_GIVE_UP_MS (5U * 60U * 1000U)

static enum be_image_state state = BE_IMAGE_CONFIRMED;
static bool cfg_ok;

void be_ota_init(int cfg_err)
{
	cfg_ok = cfg_err == 0;
	if (mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT) {
		/* MCUboot swapped this image in for a test: it swaps back unless it is confirmed */
		state = BE_IMAGE_TEST;
	} else {
		/* A confirmed image, or one written through SWD without a trailer: mark it, so that a
		 * later update finds a confirmed image in slot 0 */
		if (!boot_is_img_confirmed()) {
			printk("image written without a confirm, confirming: %d\n", boot_write_img_confirmed());
		}
		state = BE_IMAGE_CONFIRMED;
	}
	printk("image: %s\n", state == BE_IMAGE_TEST ? "test image, not confirmed" : "confirmed");
}

void be_ota_poll(void)
{
	if (state != BE_IMAGE_TEST) {
		return;
	}
	const int64_t up = k_uptime_get();
	/* A start after a watchdog or fault reset does not confirm: MCUboot swaps back at the next one */
	const enum be_reset_cause cause = be_wdt_reset_cause();
	const bool clean_start = cause != BE_RESET_WDT_FIRST && cause != BE_RESET_FATAL &&
				 cause != BE_RESET_WDT_HW;

	if (cfg_ok && clean_start && link_peer_alive() && up >= CONFIRM_AFTER_MS) {
		int err = boot_write_img_confirmed();

		printk("image confirmed: %d\n", err);
		if (err == 0) {
			state = BE_IMAGE_CONFIRMED;
		}
		return;
	}
	if (up >= NO_LINK_GIVE_UP_MS && !link_peer_alive()) {
		printk("test image: no link to the ESP32 for 5 minutes, restarting for the old image\n");
		be_cfg_system_reset();
	}
}

enum be_image_state be_ota_image_state(void)
{
	return state;
}

#else /* !CONFIG_BOOTLOADER_MCUBOOT */

void be_ota_init(int cfg_err)
{
	ARG_UNUSED(cfg_err);
}

void be_ota_poll(void)
{
}

enum be_image_state be_ota_image_state(void)
{
	return BE_IMAGE_NO_BOOTLOADER;
}

#endif
