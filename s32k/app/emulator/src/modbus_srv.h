/*
 * Modbus RTU server for SCADA / Home Assistant. Register map: s32k/app/emulator/MODBUS_MAP.md
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MODBUS_SRV_H
#define MODBUS_SRV_H

#include <stdint.h>

#define MB_MAP_VERSION 2

/* Starts the server on the "modbus0" devicetree node with the unit id/baud/parity of be_cfg. */
int modbus_srv_init(void);

#endif /* MODBUS_SRV_H */
