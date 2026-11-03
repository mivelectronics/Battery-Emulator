#include "network_status.h"

#include <WiFi.h>

#include "../utils/logging.h"  // logging, syslog_start()
#include "../wifi/wifi.h"      // wifi_connected()
#ifdef ETHERNET_W5500
#include "../ethernet/eth_w5500.h"
#endif
#include "mdns.h"              // init_mDNS()

bool network_connected() {
#ifdef ETHERNET_W5500
  if (ethernet_w5500_connected()) {
    return true;
  }
#endif
  return wifi_connected();
}

IPAddress network_localIP() {
#ifdef ETHERNET_W5500
  if (ethernet_w5500_connected()) {
    return ETH.localIP();
  }
#endif
  return WiFi.localIP();
}

void network_bring_services_up(const IPAddress& ip) {
  LOG_SET_NEXT_SEVERITY(5);  // notice
  logging.printf("Got IP address: %s\n", ip.toString().c_str());
  syslog_start();  // safe to call more than once
#ifndef SMALL_FLASH_DEVICE
  init_mDNS();
#endif
}
