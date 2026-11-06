/*
 * MPU regions of the S32K312, instead of the Zephyr default for this SoC, which maps only flash and
 * SRAM and leaves the rest to the background map. There the Cortex-M7 can access speculatively an
 * address with nothing behind it, and on this chip that hangs the bus: the core and the interrupts
 * stop, a debugger cannot halt it, and only the second stage of the watchdog brings it back, as a
 * destructive reset. Whether it happens depends on the layout of the code, see FIRMWARE_PLAN.md.
 * NXP ERR011573; the S32K3 SoC in Zephyr has the same region 0.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <zephyr/arch/arm/mpu/arm_mpu_mem_cfg.h>

static const struct arm_mpu_region mpu_regions[] = {
	/* First: every other region has a higher number and wins over it */
	MPU_REGION_ENTRY("UNMAPPED", 0, {REGION_4G | MPU_RASR_XN_Msk | P_NA_U_NA_Msk}),
	MPU_REGION_ENTRY("ITCM", 0,
			 {NORMAL_OUTER_INNER_WRITE_BACK_WRITE_READ_ALLOCATE_NON_SHAREABLE | REGION_32K |
			  P_RW_U_NA_Msk}),
	MPU_REGION_ENTRY("FLASH", CONFIG_FLASH_BASE_ADDRESS, REGION_FLASH_ATTR(REGION_FLASH_SIZE)),
	MPU_REGION_ENTRY("DTCM", 0x20000000, REGION_RAM_ATTR(REGION_64K)),
	MPU_REGION_ENTRY("SRAM", CONFIG_SRAM_BASE_ADDRESS, REGION_RAM_ATTR(REGION_SRAM_SIZE)),
	MPU_REGION_ENTRY("PERIPHERALS", 0x40000000, REGION_IO_ATTR(REGION_512M)),
	MPU_REGION_ENTRY("PPB", 0xE0000000, REGION_PPB_ATTR(REGION_1M)),
};

const struct arm_mpu_config mpu_config = {
	.num_regions = ARRAY_SIZE(mpu_regions),
	.mpu_regions = mpu_regions,
};
