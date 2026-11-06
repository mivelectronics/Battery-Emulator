/*
 * Settings of the ESP32 network side (web, MQTT, WiFi, syslog). comm_nvm.cpp reads them into these
 * variables, and on the S32K nothing else looks at them. Network features stay on the ESP32 module.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <Arduino.h>
#include <WString.h>
#include <string>

#include "src/devboard/mqtt/mqtt.h"
#include "src/devboard/network/hostname.h"
#include "src/devboard/utils/logging.h"
#include "src/devboard/wifi/wifi.h"

std::string http_username;
std::string http_password;
bool webserver_auth = false;

std::string ssid;
std::string password;
uint16_t wifi_channel = 0;
std::string passwordAP;
const char* DEFAULT_AP_PASSWORD = "";
bool wifiap_enabled = false;
bool espnow_enabled = false;
std::string espnow_peer_macs;
bool wifi_static_IP_enabled = false;

std::string custom_hostname;

std::string syslog_ip;
uint16_t syslog_port = 0;
uint8_t syslog_facility = 0;

bool mqtt_enabled = false;
bool mqtt_transmit_all_cellvoltages = false;
bool mqtt_publish_heap_metrics = false;
uint16_t mqtt_timeout_ms = 0;
uint16_t mqtt_publish_interval_ms = 0;
bool ha_autodiscovery_enabled = false;
std::string ha_autodiscovery_topic;
std::string mqtt_server;
std::string mqtt_user;
std::string mqtt_password;
int mqtt_port = 0;

uint32_t mqtt_firmware_signature(void) {
  return 0;
}
