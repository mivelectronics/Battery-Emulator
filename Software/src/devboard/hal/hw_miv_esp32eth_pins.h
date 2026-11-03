#pragma once

// Pins of the MIV ESP32ETH_8R8I that are used outside the HAL class (hal.cpp includes the board
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
// MCP23S17 (relays/inputs) on the same bus
constexpr gpio_num_t MCP23S17_CS = GPIO_NUM_32;
// Link UART to the S32K (CAN processor)
constexpr gpio_num_t LINK_TX = GPIO_NUM_17;
constexpr gpio_num_t LINK_RX = GPIO_NUM_39;
}  // namespace miv_esp32eth
