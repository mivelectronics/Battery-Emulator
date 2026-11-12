/*
 * The board's own output lines (compatible "miv,gpio-lines" with output-off-at-start) are driven to
 * their inactive level as soon as the GPIO driver runs: the outputs the Battery-Emulator core does not
 * drive (OUT5, OUT6, BMS12_EN, BMS12_EN2, BMS_POWER2) and the MultiSense control of the VNQ7140AJ.
 * The lines of the core are left alone: it takes them itself, and be_gpio_note_reset_state() must see
 * them as the reset left them. A board without such lines (FRDM-A-S32K312) compiles nothing here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <zephyr/devicetree.h>

#if DT_HAS_COMPAT_STATUS_OKAY(miv_gpio_lines)

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define OFF_AT_START(node)                                                                         \
	COND_CODE_1(DT_PROP(node, output_off_at_start), (GPIO_DT_SPEC_GET(node, gpios),), ())
#define LINES_OF(node) DT_FOREACH_CHILD_STATUS_OKAY(node, OFF_AT_START)

static const struct gpio_dt_spec off_at_start[] = {
	DT_FOREACH_STATUS_OKAY(miv_gpio_lines, LINES_OF)
};

static int board_lines_init(void)
{
	int failed = 0;

	for (size_t i = 0; i < ARRAY_SIZE(off_at_start); i++) {
		if (!gpio_is_ready_dt(&off_at_start[i]) ||
		    gpio_pin_configure_dt(&off_at_start[i], GPIO_OUTPUT_INACTIVE) != 0) {
			failed++;
		}
	}
	if (failed != 0) {
		printk("board lines: %d of %u not set off\n", failed, (unsigned int)ARRAY_SIZE(off_at_start));
	}
	return 0;
}

/* After the GPIO driver (POST_KERNEL, CONFIG_GPIO_INIT_PRIORITY), before main() */
SYS_INIT(board_lines_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
