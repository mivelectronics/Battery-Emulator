/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIV_BOOT_H
#define MIV_BOOT_H

/* Test build only (CONFIG_MIV_BOOT_SLOW_SWAP): from every watchdog feed, slows a swap down */
void miv_boot_slow_swap_step(void);

#endif /* MIV_BOOT_H */
