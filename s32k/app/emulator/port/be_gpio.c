/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_gpio.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec pins[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, be_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static uint16_t in_use;
static uint16_t outputs;
static uint16_t after_reset;

#ifdef BE_SIM
#define TRACE_LEN 24
struct trace_entry {
	uint32_t ms;
	uint8_t pin;
	uint8_t level;
};
static struct trace_entry trace[TRACE_LEN];
static uint16_t trace_n;

static void trace_add(uint8_t pin, bool level)
{
	if (trace_n < TRACE_LEN) {
		trace[trace_n++] = (struct trace_entry){k_uptime_get_32(), pin, level};
	}
}

bool be_gpio_trace_at(uint16_t n, uint8_t *pin, uint8_t *level, uint32_t *ms)
{
	if (n >= trace_n) {
		return false;
	}
	*pin = trace[n].pin;
	*level = trace[n].level;
	*ms = trace[n].ms;
	return true;
}

uint16_t be_gpio_trace_count(void)
{
	return trace_n;
}

void be_gpio_trace_clear(void)
{
	trace_n = 0;
}
#endif

void be_gpio_note_reset_state(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		gpio_flags_t flags = 0;

		if (gpio_pin_get_config_dt(&pins[i], &flags) == 0 && (flags & GPIO_OUTPUT) != 0) {
			after_reset |= BIT(i);
		}
	}
}

uint16_t be_gpio_outputs_after_reset(void)
{
	return after_reset;
}

void be_gpio_configure(uint8_t pin, bool output)
{
	if (pin >= ARRAY_SIZE(pins)) {
		return;
	}
	/* The input buffer stays on, so that an output can be read back from the pad. The pull-down on an
	 * input: the stop button is normally closed to 3.3 V and upstream reads low as pressed, so a
	 * broken wire stops the system. */
	if (gpio_pin_configure_dt(&pins[pin], output ? (GPIO_OUTPUT_LOW | GPIO_INPUT)
						     : (GPIO_INPUT | GPIO_PULL_DOWN)) == 0) {
		in_use |= BIT(pin);
		WRITE_BIT(outputs, pin, output);
	}
}

void be_gpio_write(uint8_t pin, bool high)
{
	if (pin >= ARRAY_SIZE(pins) || (outputs & BIT(pin)) == 0) {
		return;
	}
#ifdef BE_SIM
	if (gpio_pin_get_raw(pins[pin].port, pins[pin].pin) != (int)high) {
		trace_add(pin, high);
	}
#endif
	(void)gpio_pin_set_raw(pins[pin].port, pins[pin].pin, high ? 1 : 0);
}

int be_gpio_read(uint8_t pin)
{
	if (pin >= ARRAY_SIZE(pins) || (in_use & BIT(pin)) == 0) {
		return 0;
	}
	return gpio_pin_get_raw(pins[pin].port, pins[pin].pin);
}

uint16_t be_gpio_levels(void)
{
	uint16_t levels = 0;

	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		if (be_gpio_read((uint8_t)i) > 0) {
			levels |= BIT(i);
		}
	}
	return levels;
}

void be_gpio_safe_off(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		if ((outputs & BIT(i)) != 0) {
			(void)gpio_pin_configure_dt(&pins[i], GPIO_DISCONNECTED);
		}
	}
	outputs = 0;
	in_use = 0;
}
