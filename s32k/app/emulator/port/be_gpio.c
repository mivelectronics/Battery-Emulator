/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_gpio.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER_NODE, be_gpios)
/*
 * The pins listed in the application's board overlay (FRDM-A-S32K312, pins on headers). Levels as
 * on the pad; an input gets the internal pull-down.
 */
static const struct gpio_dt_spec pins[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, be_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};
#define PIN_OUTPUT_FLAGS (GPIO_OUTPUT_LOW | GPIO_INPUT)
#define PIN_INPUT_FLAGS  (GPIO_INPUT | GPIO_PULL_DOWN)
#define PIN_SET(i, v)    gpio_pin_set_raw(pins[i].port, pins[i].pin, (v))
#define PIN_GET(i)       gpio_pin_get_raw(pins[i].port, pins[i].pin)
#else
/*
 * The lines of the board, named in its devicetree (be-lines: phandles to nodes with a gpios
 * property, MIV be_main). Levels in the logical sense of the line's flags (the stop input is active
 * low there); the bias comes from the board (external resistors), none is added.
 */
#define BE_LINE_SPEC(node, prop, idx) GPIO_DT_SPEC_GET(DT_PHANDLE_BY_IDX(node, prop, idx), gpios)
static const struct gpio_dt_spec pins[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, be_lines, BE_LINE_SPEC, (,))
};
#define PIN_OUTPUT_FLAGS (GPIO_OUTPUT_INACTIVE | GPIO_INPUT)
#define PIN_INPUT_FLAGS  GPIO_INPUT
#define PIN_SET(i, v)    gpio_pin_set_dt(&pins[i], (v))
#define PIN_GET(i)       gpio_pin_get_dt(&pins[i])
#endif

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
	/* The input buffer stays on, so that an output can be read back from the pad. Upstream reads low
	 * as pressed, and the stop button is normally closed: on the FRDM to 3.3 V with the internal
	 * pull-down here, on be_main to GND against the board's pull-up with the line active low. Either
	 * way a broken wire stops the system. */
	if (gpio_pin_configure_dt(&pins[pin], output ? PIN_OUTPUT_FLAGS : PIN_INPUT_FLAGS) == 0) {
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
	if (PIN_GET(pin) != (int)high) {
		trace_add(pin, high);
	}
#endif
	(void)PIN_SET(pin, high ? 1 : 0);
}

int be_gpio_read(uint8_t pin)
{
	if (pin >= ARRAY_SIZE(pins) || (in_use & BIT(pin)) == 0) {
		return 0;
	}
	return PIN_GET(pin);
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
