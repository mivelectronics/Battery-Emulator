/*
 * The hardware watchdog and what started this run of the firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_WDT_H
#define BE_WDT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum be_reset_cause {
	BE_RESET_POWER = 0,   /* power on, or a reset that clears the RAM */
	BE_RESET_REQUESTED,   /* a restart we were asked for: Modbus, the web page */
	BE_RESET_WDT_FIRST,   /* watchdog, first stage: interrupts alive, firmware closed the outputs */
	BE_RESET_FATAL,       /* CPU fault, assert, kernel panic */
	BE_RESET_WDT_HW,      /* watchdog, second stage: the chip reset itself, interrupts were dead */
	BE_RESET_EXTERNAL,    /* the reset pin */
	BE_RESET_OTHER,
};

/* First thing in main: reads the reset flags and clears them */
void be_wdt_boot(void);

/* After everything else is up. Starts the watchdog. */
void be_wdt_start(void);

/* From the main loop, to prove it still runs */
void be_wdt_main_alive(void);

/* Turns the outputs off and restarts. For the fault paths; does not return. */
void be_wdt_fail(enum be_reset_cause cause);

enum be_reset_cause be_wdt_reset_cause(void);
uint32_t be_wdt_resets_in_row(void);
bool be_wdt_running(void);

/* MC_RGM DES and FES as they were at the start of this run */
uint32_t be_wdt_flags_des(void);
uint32_t be_wdt_flags_fes(void);

/* Too many watchdog resets in a row: leave the contactors open. A restart we are asked for clears it. */
bool be_wdt_contactors_locked_out(void);

/* Uptime of the previous run when be_wdt_fail() closed the outputs, ms */
uint32_t be_wdt_last_off_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* BE_WDT_H */
