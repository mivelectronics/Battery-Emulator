# SPDX-License-Identifier: Apache-2.0
# Flashing: on-board OpenSDA runs PEmicro firmware (Embedded Multilink).
# Zephyr has no PEmicro runner; use tools/flash_pe.sh (pegdbserver + gdb load).
board_runner_args(jlink "--device=S32K312")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
