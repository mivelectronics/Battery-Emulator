/*
 * The outputs and inputs of the Battery-Emulator core: upstream names them with the numbers in
 * hw_miv_s32k.h, these are the indices of the be-gpios list in the board overlay.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_GPIO_H
#define BE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before anything configures a pin: notes which ones are still driven after the reset */
void be_gpio_note_reset_state(void);

/* Bit per pin: which were output enabled at the start. A reset that releases the pads gives 0. */
uint16_t be_gpio_outputs_after_reset(void);

void be_gpio_configure(uint8_t pin, bool output);
void be_gpio_write(uint8_t pin, bool high);
int be_gpio_read(uint8_t pin);

/* Bit per pin: the level on the pad, 0 for pins not in use */
uint16_t be_gpio_levels(void);

/*
 * Turns every output driver off, the state a reset leaves them in. From any context, including a fault
 * handler. The pull-down on the driver then opens the contactors.
 */
void be_gpio_safe_off(void);

#ifdef BE_SIM
/* Changes of the outputs, oldest first. Returns false past the end. */
bool be_gpio_trace_at(uint16_t n, uint8_t *pin, uint8_t *level, uint32_t *ms);
uint16_t be_gpio_trace_count(void);
void be_gpio_trace_clear(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BE_GPIO_H */
