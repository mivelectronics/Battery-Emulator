#include "eth_w5500.h"

#ifdef ETHERNET_W5500

#include <SPI.h>

#include "../hal/hal.h"
#include "../hal/hw_miv_esp32eth_pins.h"
#include "../network/hostname.h"        // active_hostname()
#include "../network/network_status.h"  // network_bring_services_up()
#include "../utils/logging.h"

static volatile bool eth_has_ip = false;

static void onEthEvent(WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      // The netif exists only from here on, so the hostname is applied now.
      ETH.setHostname(active_hostname().c_str());
      logging.println("Ethernet (W5500) started");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      logging.printf("Ethernet link up, %u Mbit/s %s duplex\n", ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      eth_has_ip = true;
      network_bring_services_up(ETH.localIP());  // log IP + syslog + mDNS
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      if (eth_has_ip) {
        logging.println("Ethernet link down");
      }
      eth_has_ip = false;
      break;
    default:
      break;
  }
}

void init_Ethernet_W5500() {
  using namespace miv_esp32eth;

  esp32hal->alloc_pins("Ethernet W5500", W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS, W5500_INT, W5500_RST);

  // The MCP23S17 shares the bus: keep it deselected.
  pinMode(MCP23S17_CS, OUTPUT);
  digitalWrite(MCP23S17_CS, HIGH);

  WiFi.onEvent(onEthEvent, ARDUINO_EVENT_ETH_START);
  WiFi.onEvent(onEthEvent, ARDUINO_EVENT_ETH_CONNECTED);
  WiFi.onEvent(onEthEvent, ARDUINO_EVENT_ETH_GOT_IP);
  WiFi.onEvent(onEthEvent, ARDUINO_EVENT_ETH_LOST_IP);
  WiFi.onEvent(onEthEvent, ARDUINO_EVENT_ETH_DISCONNECTED);

  // SPI3_HOST is VSPI on the classic ESP32. PHY address 1 is the W5500 default.
  const bool ok = ETH.begin(ETH_PHY_W5500, 1, W5500_CS, W5500_INT, W5500_RST, SPI3_HOST, W5500_SCK, W5500_MISO,
                            W5500_MOSI, 20);
  if (!ok) {
    logging.println("Ethernet (W5500) init failed");
  }
}

bool ethernet_w5500_connected() {
  return eth_has_ip;
}

#endif  // ETHERNET_W5500
