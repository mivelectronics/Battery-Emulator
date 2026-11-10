/*
 * Copyright 2025 NXP
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/platform/hooks.h>
#include <soc.h>

#if CONFIG_BOARD_EARLY_INIT_HOOK
void board_early_init_hook(void)
{
	/*
	 * Started by a bootloader (MCUboot), the core still runs from the PLL. The clock driver
	 * (mc_cgm_init) first switches FXOSC and the PLL off to set them up again, which stops the
	 * core clock unless the core already runs from FIRC, as it does after a reset. So move the
	 * core clock mux to the safe clock (FIRC) first and power the PLL down, so that it does not
	 * lose its lock when FXOSC goes off; the driver then starts both and switches to the PLL as
	 * on a cold start. After a reset the mux already selects FIRC and nothing is done.
	 */
	if ((MC_CGM->MUX_0_CSS & MC_CGM_MUX_0_CSS_SELSTAT_MASK) != 0U) {
		MC_CGM->MUX_0_CSC |= MC_CGM_MUX_0_CSC_SAFE_SW_MASK;
		while ((MC_CGM->MUX_0_CSS & MC_CGM_MUX_0_CSS_SAFE_SW_MASK) == 0U) {
		}
		for (uint32_t i = 0U; i < PLL_PLLODIV_COUNT; i++) {
			PLL->PLLODIV[i] &= ~PLL_PLLODIV_DE_MASK;
		}
		PLL->PLLCR |= PLL_PLLCR_PLLPD_MASK;
	}

#if defined(CONFIG_CLEAR_DESTRUCTIVE_RESET_FLAG) && (CONFIG_CLEAR_DESTRUCTIVE_RESET_FLAG)
	MC_RGM->DES = CONFIG_CLEAR_DESTRUCTIVE_RESET_FLAG;
#endif /* CONFIG_CLEAR_DESTRUCTIVE_RESET_FLAG */

#if defined(CONFIG_CLEAR_FUNCTIONAL_RESET_FLAG) && (CONFIG_CLEAR_FUNCTIONAL_RESET_FLAG)
	MC_RGM->FES = CONFIG_CLEAR_FUNCTIONAL_RESET_FLAG;
#endif
}

#endif /* CONFIG_BOARD_EARLY_INIT_HOOK */
