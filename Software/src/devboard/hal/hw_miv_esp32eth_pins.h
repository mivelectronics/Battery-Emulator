#pragma once

// Pins of the MIV ESP32ETH_8R8I and of the KMP ProDINo ESP32 Ethernet v15 (-D MIV_PRODINO_V15) that are
// used outside the HAL class (hal.cpp includes the board
// HAL header inside init_hal(), so its class is local to that function).

#include <driver/gpio.h>

namespace miv_esp32eth {
// W5500 Ethernet on VSPI
constexpr gpio_num_t W5500_SCK = GPIO_NUM_18;
constexpr gpio_num_t W5500_MISO = GPIO_NUM_19;
constexpr gpio_num_t W5500_MOSI = GPIO_NUM_23;
constexpr gpio_num_t W5500_CS = GPIO_NUM_33;
constexpr gpio_num_t W5500_INT = GPIO_NUM_5;
constexpr gpio_num_t W5500_RST = GPIO_NUM_12;
// I/O expander on the same bus, held deselected: MCP23S17 (ESP32ETH_8R8I) or MCP23S08 (ProDINo v15), CS 32 on both
constexpr gpio_num_t MCP23S17_CS = GPIO_NUM_32;
// Link UART to the S32K (CAN processor): TX IO17 (J11 pin 4) on both boards
constexpr gpio_num_t LINK_TX = GPIO_NUM_17;
#ifdef MIV_PRODINO_V15
// ProDINo v15: RX on IO13 (J14 pin 11); I39 is not on a connector of this board
constexpr gpio_num_t LINK_RX = GPIO_NUM_13;
#else
constexpr gpio_num_t LINK_RX = GPIO_NUM_39;  // ESP32ETH_8R8I: J11 pin 7
#endif
}  // namespace miv_esp32eth
