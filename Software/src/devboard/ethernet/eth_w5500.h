#pragma once

// Ethernet over a WIZnet W5500 on SPI (MIV ESP32ETH_8R8I). Modelled on the RMII Ethernet module of
// upstream PR #2833 so the two can be merged when that PR lands; built only with -D ETHERNET_W5500.

#ifdef ETHERNET_W5500

#include <ETH.h>
#include <WiFi.h>

// Start the W5500 with the pins from the board HAL. Call before init_WiFi().
void init_Ethernet_W5500();

// True once the link is up and the interface has an IP address.
bool ethernet_w5500_connected();

#endif  // ETHERNET_W5500
