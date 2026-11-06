# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree boards live in s32k/boards
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(be_s32k_link_bridge)

# The link protocol is shared byte for byte with the ESP32 firmware
set(BE_SOFTWARE ${CMAKE_CURRENT_LIST_DIR}/../../../Software)
target_include_directories(app PRIVATE ${BE_SOFTWARE}/src/communication/link)
target_sources(app PRIVATE src/main.c ${BE_SOFTWARE}/src/communication/link/be_link.c)
