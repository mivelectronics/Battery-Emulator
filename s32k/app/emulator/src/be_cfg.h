/*
 * Configuration of the S32K emulator (variant A+): the S32K is the source of truth for the
 * settings and keeps them in NVS. The ESP32 (web UI) and Modbus (SCADA/HA) both edit them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_CFG_H
#define BE_CFG_H

#include <stdint.h>

#define BE_CFG_VERSION 2
#define BE_NUM_CAN     3

struct be_cfg_can {
	uint16_t nominal_kbps; /* arbitration bit rate */
	uint16_t data_kbps;    /* CAN FD data bit rate, 0 = classic CAN */
	uint8_t enabled;
	uint8_t reserved[3];
};

struct be_cfg {
	uint16_t version; /* BE_CFG_VERSION */
	uint16_t reserved0;
	uint32_t revision; /* increments on every save */

	struct be_cfg_can can[BE_NUM_CAN];

	/* Modbus RTU server (takes effect after a reboot) */
	uint8_t mb_unit_id;
	uint8_t mb_parity; /* 0 none, 1 odd, 2 even */
	uint16_t mb_baud_div100;

	/* Emulator selection, indices of the upstream Battery-Emulator enums. After a reboot. Battery type
	 * 0 turns the core off and bridges CAN to the ESP32 over the link. Second battery: same type as
	 * the first, any value other than 0 enables it. */
	uint16_t battery_type;
	uint16_t battery2_type;
	uint16_t inverter_type;
	uint16_t reserved1;

	/* Which CAN channel each one uses (0 = CAN0, 1 = CAN1, 2 = CAN4). After a reboot. */
	uint8_t battery_iface;
	uint8_t battery2_iface;
	uint8_t inverter_iface;
	uint8_t soc_scaling; /* 1 = inverter sees the SOC scaled between the limits below */

	/* Limits, same meaning and defaults as upstream's battery_settings. After a reboot. */
	uint16_t max_percentage; /* SOC 0.01 %, top of the scaled window */
	int16_t min_percentage;  /* SOC 0.01 %, bottom of the scaled window */
	uint16_t max_charge_dA;
	uint16_t max_discharge_dA;
};

/* Active configuration (what the firmware runs with) */
extern struct be_cfg be_cfg;

void be_cfg_defaults(struct be_cfg *cfg);

/* Mounts settings, loads the stored configuration or falls back to the defaults. */
int be_cfg_init(void);

/* Stores *cfg as the new configuration (revision is incremented) and makes it active. */
int be_cfg_save(const struct be_cfg *cfg);

/* Re-reads the stored configuration into *cfg (defaults if nothing stored). */
int be_cfg_reload(struct be_cfg *cfg);

/* 0 if the configuration is consistent, negative errno otherwise */
int be_cfg_validate(const struct be_cfg *cfg);

/* Restarts the chip. On S32K3 / MCX E31x the Arm SYSRESETREQ used by sys_reboot() leaves the part
 * hung (seen on FRDM-A-S32K312); a functional reset through MC_ME works. Does not return. */
void be_cfg_system_reset(void);
void be_cfg_system_reset_in(uint32_t ms);

#endif /* BE_CFG_H */
