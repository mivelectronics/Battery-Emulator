/*
 * The Battery-Emulator core (upstream drivers, datalayer, safety) on the S32K.
 *
 * With a battery type set in be_cfg the S32K is the emulator: the core owns the CAN channels. With
 * battery type 0 the core stays off and the channels are bridged to the ESP32 over the link.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_CORE_H
#define BE_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A received CAN frame on its way from the interrupt to the core */
struct be_can_rx {
	uint32_t id;
	uint8_t iface;
	uint8_t ext;
	uint8_t fd;
	uint8_t len;
	uint8_t data[64];
};

/* be_core.cpp: runs in the core thread */
void be_core_setup(void);
void be_core_tick(void);
void be_core_print_status(void);
int be_core_reg_read(uint16_t addr, uint16_t *value);

/* be_core_run.c */
extern uint32_t be_core_rx_drops;
extern uint32_t be_core_tick_max_us;
uint16_t be_core_stack_free(void);
int be_core_start(void); /* after can_io_init(); does nothing while the battery type is 0 */
bool be_core_active(void);
bool be_core_rx_pop(struct be_can_rx *frame);
void be_core_rx_isr(uint8_t iface, const void *zephyr_can_frame);

#ifdef __cplusplus
}
#endif

#endif /* BE_CORE_H */
