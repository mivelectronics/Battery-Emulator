/*
 * Force-included before comm_nvm.cpp only. It reads settings of the web server, which does not exist on
 * the S32K, and its header needs ESPAsyncWebServer. The guard keeps that header out; these are the few
 * names comm_nvm.cpp wants from it. Defined in port_network_stubs.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define WEBSERVER_H
#include <string>
#include "src/devboard/hal/hal.h"
extern std::string http_username;
extern std::string http_password;
extern bool webserver_auth;
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
