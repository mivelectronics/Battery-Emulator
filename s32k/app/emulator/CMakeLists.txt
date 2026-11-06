# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree boards live in s32k/boards
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(be_s32k_emulator)

# The link protocol is shared byte for byte with the ESP32 firmware
set(BE_SOFTWARE ${CMAKE_CURRENT_LIST_DIR}/../../../Software)
target_include_directories(app PRIVATE src ${BE_SOFTWARE}/src/communication/link)
target_sources(app PRIVATE
  src/main.c
  src/be_cfg.c
  src/be_mpu.c
  src/be_wdt.c
  src/can_io.c
  src/link.c
  src/modbus_srv.c
  ${BE_SOFTWARE}/src/communication/link/be_link.c
)

# ---- The Battery-Emulator core ------------------------------------------------------------------
# Upstream sources, not copied. The list of what links without the ESP32 network, display and SD card
# code is the one the host unit tests keep (test/CMakeLists.txt), and the test/emul headers stand in
# for Arduino the same way. port/ comes first in the include path: its Preferences.h replaces theirs.
set(BE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
file(STRINGS ${BE_ROOT}/test/CMakeLists.txt BE_TEST_LINES REGEX "^ *\\.\\./Software/src/.*\\.cpp")
set(BE_CORE_SOURCES)
foreach(line ${BE_TEST_LINES})
  string(STRIP "${line}" rel)
  list(APPEND BE_CORE_SOURCES ${BE_ROOT}/test/${rel})
endforeach()
list(LENGTH BE_CORE_SOURCES BE_CORE_COUNT)
if(BE_CORE_COUNT LESS 50)
  message(FATAL_ERROR "Found only ${BE_CORE_COUNT} Battery-Emulator sources in test/CMakeLists.txt")
endif()
list(APPEND BE_CORE_SOURCES
  ${BE_SOFTWARE}/src/communication/nvm/comm_nvm.cpp
  ${BE_SOFTWARE}/src/communication/link/be_snapshot.cpp
  ${BE_SOFTWARE}/src/communication/equipmentstopbutton/comm_equipmentstopbutton.cpp
  ${BE_SOFTWARE}/src/devboard/utils/debounce_button.cpp
  ${BE_ROOT}/test/emul/serial.cpp
  ${BE_ROOT}/test/emul/neopixel_show.cpp
  ${BE_ROOT}/test/emul/freertos/FreeRTOS.cpp
  ${BE_ROOT}/test/emul/driver/i2c_master.cpp
)

# The calculated values logic lives in Software.cpp, which cannot be compiled here. Take it from there
# so it never drifts from upstream. Fails loudly if upstream moves the markers.
file(READ ${BE_SOFTWARE}/Software.cpp BE_SOFTWARE_CPP)
string(FIND "${BE_SOFTWARE_CPP}" "struct InstallationLimits {" BE_CALC_BEGIN)
string(FIND "${BE_SOFTWARE_CPP}" "void check_reset_reason() {" BE_CALC_END)
if(BE_CALC_BEGIN LESS 0 OR BE_CALC_END LESS BE_CALC_BEGIN)
  message(FATAL_ERROR "Calculated values section not found in Software.cpp")
endif()
math(EXPR BE_CALC_LEN "${BE_CALC_END} - ${BE_CALC_BEGIN}")
string(SUBSTRING "${BE_SOFTWARE_CPP}" ${BE_CALC_BEGIN} ${BE_CALC_LEN} BE_CALC)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/be_core_gen/be_core_calc.inc CONTENT "${BE_CALC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${BE_SOFTWARE}/Software.cpp ${BE_ROOT}/test/CMakeLists.txt)

zephyr_library_named(be_core)
zephyr_library_sources(${BE_CORE_SOURCES} port/be_core.cpp port/port_arduino.cpp port/port_can.cpp
  port/port_network_stubs.cpp)
zephyr_library_include_directories(port src ${CMAKE_CURRENT_BINARY_DIR}/be_core_gen
  ${BE_ROOT}/test/emul ${BE_SOFTWARE} ${BE_SOFTWARE}/src/communication/link)
zephyr_library_compile_definitions(UNIT_TEST ESP32 HW_MIV_S32K COMMON_IMAGE)
zephyr_library_compile_options(-w -include ${CMAKE_CURRENT_LIST_DIR}/port/port_prefix.h)
set_source_files_properties(${BE_SOFTWARE}/src/communication/nvm/comm_nvm.cpp PROPERTIES
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/port/comm_nvm_prefix.h")

target_link_libraries(app PUBLIC be_core)
target_include_directories(app PRIVATE port ${BE_SOFTWARE}/src/communication/link)
target_compile_definitions(app PRIVATE HW_MIV_S32K)
target_sources(app PRIVATE port/be_core_run.c port/port_time.c port/be_kv.c port/be_link_cmd.c port/be_gpio.c)

# Bench simulator: west build ... -- -DBE_SIM=1
if(BE_SIM)
  target_sources(app PRIVATE port/sim_battery.c)
  target_compile_definitions(app PRIVATE BE_SIM)
endif()
