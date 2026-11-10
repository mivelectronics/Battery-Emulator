/*
 * The hardware watchdog (SWT0). A periodic check in the timer interrupt feeds it only while both the
 * core thread and the main thread make progress.
 *
 * Two stages. No feed for WDT_TIMEOUT_MS: the SWT interrupt turns the outputs off and restarts. If the
 * interrupts are dead too, nothing clears its flag and the second timeout resets the chip in hardware,
 * which also releases the pads.
 *
 * The registers directly, not the Zephyr SWT driver: with that driver in the build the chip stopped dead
 * 40 s after every start, watchdog running or not, interrupts and debugger access gone (measured on
 * FRDM-A-S32K312, cause not known). It writes SWT_CR during the kernel start, which this code does not.
 *
 * A firmware that crashes at the same place every run would otherwise cycle the contactors and heat the
 * precharge resistor with every start, so after IN_ROW_LOCKOUT resets in a row they stay open.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_wdt.h"

#include <zephyr/cache.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <soc.h>

#include "be_cfg.h"
#include "be_core.h"
#include "be_gpio.h"

#define WDT_TIMEOUT_MS    500
#define CHECK_MS          100
#define CORE_STALE_CHECKS 2                /* the core ticks every 1 ms, its longest tick is about 3 ms */
#define CORE_START_MS     5000             /* its setup, until the first tick */
#define MAIN_STALE_MS     5000             /* the main thread may wait for a flash erase */
#define IN_ROW_LOCKOUT    3
#define IN_ROW_CLEAR_MS   (10U * 60U * 1000U)

#define SWT_CLOCK_HZ     32000U /* SIRC */
#define SWT_SERVICE_KEY1 0xA602U
#define SWT_SERVICE_KEY2 0xB480U

#define KEEP_MAGIC 0x57445430U

/* Survives a functional reset: the RAM is only initialised after a destructive one */
static __noinit struct {
	uint32_t magic;
	uint32_t in_row;
	uint32_t intent;
	uint32_t off_ms;
} keep;

static enum be_reset_cause cause;
static uint32_t last_off_ms;
static uint32_t resets_in_row;
static bool running;
static uint32_t flags_des, flags_fes;
static volatile uint32_t main_beat_ms;

/* A reset drops what the data cache holds, so every change goes to the RAM at once */
static void keep_store(void)
{
	(void)sys_cache_data_flush_range(&keep, sizeof(keep));
}

void be_wdt_fail(enum be_reset_cause why)
{
	be_gpio_safe_off();
	keep.intent = why;
	keep.off_ms = k_uptime_get_32();
	keep_store();
	be_cfg_system_reset();
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);
	be_wdt_fail(BE_RESET_FATAL);
}

static void on_first_timeout(const void *arg)
{
	ARG_UNUSED(arg);
	be_wdt_fail(BE_RESET_WDT_FIRST);
}

static void check_handler(struct k_timer *timer)
{
	static uint32_t last_ticks;
	static uint8_t stale_checks;
	uint32_t now = k_uptime_get_32();
	uint32_t ticks = be_core_ticks;
	bool core_ok = true;

	ARG_UNUSED(timer);
	if (be_core_active()) {
		if (ticks == 0) {
			core_ok = now < CORE_START_MS;
		} else {
			stale_checks = ticks == last_ticks ? stale_checks + 1 : 0;
			core_ok = stale_checks < CORE_STALE_CHECKS;
		}
		last_ticks = ticks;
	}
	if (core_ok && now - main_beat_ms < MAIN_STALE_MS) {
		SWT_0->SR = SWT_SERVICE_KEY1;
		SWT_0->SR = SWT_SERVICE_KEY2;
	}
	if (now >= IN_ROW_CLEAR_MS && keep.in_row != 0) {
		keep.in_row = 0;
		keep_store();
	}
}
static K_TIMER_DEFINE(check_timer, check_handler, NULL);

void be_wdt_boot(void)
{
	uint32_t des = MC_RGM->DES;
	uint32_t fes = MC_RGM->FES;

	flags_des = des;
	flags_fes = fes;
	printk("reset flags: destructive 0x%08x functional 0x%08x\n", des, fes);
	/* The flags are write-1-to-clear and accumulate; clear them so the next start reports only its
	 * own cause */
	MC_RGM->DES = des;
	MC_RGM->FES = fes;

	be_gpio_note_reset_state();

	if (des != 0 || keep.magic != KEEP_MAGIC) {
		cause = BE_RESET_POWER;
		keep.magic = KEEP_MAGIC;
		keep.in_row = 0;
		keep.off_ms = 0;
	} else if ((fes & MC_RGM_FES_SWT0_RST_MASK) != 0) {
		cause = BE_RESET_WDT_HW;
	} else if ((fes & MC_RGM_FES_SW_FUNC_MASK) != 0) {
		cause = keep.intent == BE_RESET_WDT_FIRST || keep.intent == BE_RESET_FATAL
				? (enum be_reset_cause)keep.intent
				: BE_RESET_REQUESTED;
	} else if ((fes & MC_RGM_FES_F_EXR_MASK) != 0) {
		cause = BE_RESET_EXTERNAL;
	} else {
		cause = BE_RESET_OTHER;
	}

	if (cause == BE_RESET_WDT_HW || cause == BE_RESET_WDT_FIRST || cause == BE_RESET_FATAL) {
		keep.in_row++;
		last_off_ms = keep.off_ms;
	} else {
		keep.in_row = 0;
	}
	keep.off_ms = 0;
	resets_in_row = keep.in_row;
	keep.intent = BE_RESET_REQUESTED;
	keep_store();
	printk("reset cause %d, %u in a row\n", cause, resets_in_row);
}

void be_wdt_start(void)
{
	IRQ_CONNECT(SWT0_IRQn, 0, on_first_timeout, NULL, 0);
	irq_enable(SWT0_IRQn);

	/* MCUboot may have started it (10 s, no interrupt stage): the timeout changes only while it is off */
	SWT_0->CR &= ~SWT_CR_WEN_MASK;
	SWT_0->TO = SWT_CLOCK_HZ / 1000U * WDT_TIMEOUT_MS;
	/* FRZ: not while a debugger holds the CPU. ITR: the first timeout is an interrupt */
	SWT_0->CR |= SWT_CR_FRZ_MASK | SWT_CR_ITR_MASK | SWT_CR_WEN_MASK;

	running = true;
	main_beat_ms = k_uptime_get_32();
	k_timer_start(&check_timer, K_MSEC(CHECK_MS), K_MSEC(CHECK_MS));
	printk("watchdog running, %d ms\n", WDT_TIMEOUT_MS);
}

void be_wdt_main_alive(void)
{
	main_beat_ms = k_uptime_get_32();
}

enum be_reset_cause be_wdt_reset_cause(void)
{
	return cause;
}

uint32_t be_wdt_resets_in_row(void)
{
	return resets_in_row;
}

uint32_t be_wdt_flags_des(void)
{
	return flags_des;
}

uint32_t be_wdt_flags_fes(void)
{
	return flags_fes;
}

bool be_wdt_running(void)
{
	return running;
}

bool be_wdt_contactors_locked_out(void)
{
	return resets_in_row >= IN_ROW_LOCKOUT;
}

uint32_t be_wdt_last_off_ms(void)
{
	return last_off_ms;
}
