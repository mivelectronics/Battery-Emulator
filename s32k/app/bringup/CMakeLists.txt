# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree boards (frdm_a_s32k312) live in fw/s32k/boards
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(be_s32k_bringup)

target_sources(app PRIVATE src/main.c)
