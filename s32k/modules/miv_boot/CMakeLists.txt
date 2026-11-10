# SPDX-License-Identifier: GPL-3.0-or-later

if(CONFIG_MIV_BOOT_HOOKS)
  zephyr_library()
  zephyr_library_sources(
    miv_boot_wdt.c
    # The same MPU map as the application
    ${CMAKE_CURRENT_LIST_DIR}/../../app/emulator/src/be_mpu.c
  )
endif()
