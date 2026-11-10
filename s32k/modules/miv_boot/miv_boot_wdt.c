/*
 * MCUboot's watchdog on the S32K312: SWT0 through its registers, as the application's be_wdt.c does,
 * instead of the Zephyr SWT driver. Replaces the weak mcuboot_watchdog_setup()/mcuboot_watchdog_feed().
 *
 * 10 s, reset on the first timeout (no interrupt stage here), not locked: the application's
 * be_wdt_start() takes it over and sets its own timeout. Stopped while a debugger holds the core.
 * The setup also turns the reset escalation off.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <soc.h>

#define SWT_CLOCK_HZ     32000U /* SIRC */
#define BOOT_TIMEOUT_MS  10000U
#define SWT_SERVICE_KEY1 0xA602U
#define SWT_SERVICE_KEY2 0xB480U

void mcuboot_watchdog_setup(void)
{
	/*
	 * Reset escalation off (threshold 0), as the application does in main(): with the default of 15
	 * the chip stays in reset after some number of restarts in a row, which a bad image would cause.
	 */
	MC_RGM->DRET = 0;
	MC_RGM->FRET = 0;

	SWT_0->CR &= ~SWT_CR_WEN_MASK;
	SWT_0->TO = SWT_CLOCK_HZ / 1000U * BOOT_TIMEOUT_MS;
	SWT_0->CR = (SWT_0->CR & ~SWT_CR_ITR_MASK) | SWT_CR_FRZ_MASK | SWT_CR_WEN_MASK;
}

void mcuboot_watchdog_feed(void)
{
	SWT_0->SR = SWT_SERVICE_KEY1;
	SWT_0->SR = SWT_SERVICE_KEY2;
}
