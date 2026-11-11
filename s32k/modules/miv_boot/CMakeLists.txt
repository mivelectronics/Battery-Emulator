# SPDX-License-Identifier: GPL-3.0-or-later

if(CONFIG_MIV_BOOT_HOOKS)
  zephyr_library()
  zephyr_library_sources(
    miv_boot_wdt.c
    # The same MPU map as the application
    ${CMAKE_CURRENT_LIST_DIR}/../../app/emulator/src/be_mpu.c
  )
  if(CONFIG_MIV_BOOT_SLOW_SWAP)
    zephyr_library_sources(miv_boot_slow_swap.c)
    zephyr_library_include_directories(${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include)
  endif()
endif()
