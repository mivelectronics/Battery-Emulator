# SPDX-License-Identifier: GPL-3.0-or-later
# SWD on J14 (10-pin Cortex debug). The FRDM is flashed through its on-board PEmicro OpenSDA with
# tools/flash_pe.sh; this board needs an external probe (PEmicro Multilink or J-Link) on J14.
board_runner_args(jlink "--device=S32K312")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
