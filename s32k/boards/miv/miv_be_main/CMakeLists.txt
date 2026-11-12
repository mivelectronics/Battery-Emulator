# SPDX-License-Identifier: GPL-3.0-or-later
# Same chip and start-up as the FRDM-A-S32K312: its board.c (clock hand-over from MCUboot) and boot
# header are used as they are, not copied.
set(FRDM_DIR ${CMAKE_CURRENT_LIST_DIR}/../../nxp/frdm_a_s32k312)

zephyr_library_sources(${FRDM_DIR}/board.c)
if(CONFIG_BOARD_NXP_MCXE31X_BOOT_HEADER)
  zephyr_library_sources(${FRDM_DIR}/boot_header/boot_header.c)
  zephyr_library_include_directories(${FRDM_DIR}/boot_header)
endif()
