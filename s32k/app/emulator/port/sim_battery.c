/*
 * Bench simulator, see sim_battery.h. The battery sends the periodic frames the Kia/Hyundai 64 driver
 * reads and answers its UDS polls with a capture from a real battery (test/can_log_based/can_logs/
 * 17_Kia64_UDSpolls.txt). The inverter asks with 0x4200 like a Pylon inverter does, and the answer
 * 0x4210 is kept so that the test can check the whole path from battery frame to inverter frame.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sim_battery.h"

#include <string.h>
#include <zephyr/kernel.h>

#include "be_cfg.h"
#include "can_io.h"
#include "sim_kona_uds.inc"

/* The values the periodic frames carry; the test expects these */
#define SIM_SOC_RAW      130   /* 0x542, 0.5 % per bit: 65 % */
#define SIM_CHARGE_RAW   10000 /* 0x594: /2 * 10 W = 50 kW */
#define SIM_DISCHG_RAW   12000 /* 0x594: /2 * 10 W = 60 kW */
#define SIM_VOLTAGE_DV   3509  /* 0x595 */
#define SIM_AMPS_DA      50    /* 0x595: driver inverts the sign */
#define SIM_12V_DV       130   /* 0x596 */
#define SIM_TEMP_MIN_C   20    /* 0x596 */
#define SIM_TEMP_MAX_C   25    /* 0x596 */

/* One UDS exchange in progress per CAN channel */
struct sim_uds_state {
	const struct sim_uds_response *pending;
	uint8_t next;
	int64_t next_cf;
};
static struct sim_uds_state uds[BE_NUM_CAN];
static int64_t next_100ms, next_1s;
static uint8_t inverter_421x[8];
static uint32_t inverter_421x_count;

static void send8(uint8_t iface, uint32_t id, const uint8_t d[8])
{
	(void)can_io_send_raw(iface, id, false, false, 8, d);
}

static void send_battery_frames(uint8_t iface)
{
	const uint8_t f542[8] = {SIM_SOC_RAW};
	const uint8_t f594[8] = {SIM_CHARGE_RAW & 0xFF, SIM_CHARGE_RAW >> 8, SIM_DISCHG_RAW & 0xFF,
				 SIM_DISCHG_RAW >> 8, 0, SIM_SOC_RAW};
	const uint8_t f595[8] = {0, 0, 0, 0, SIM_AMPS_DA & 0xFF, SIM_AMPS_DA >> 8,
				 SIM_VOLTAGE_DV & 0xFF, SIM_VOLTAGE_DV >> 8};
	const uint8_t f596[8] = {0, SIM_12V_DV, 0, 0, 0, 0, SIM_TEMP_MIN_C, SIM_TEMP_MAX_C};
	const uint8_t f5d5[8] = {0, 0, 0, 100};

	send8(iface, 0x542, f542);
	send8(iface, 0x594, f594);
	send8(iface, 0x595, f595);
	send8(iface, 0x596, f596);
	send8(iface, 0x5D5, f5d5);
}

void sim_battery_start(void)
{
	(void)can_io_set_loopback(be_cfg.battery_iface, true);
	if (be_cfg.battery2_type != 0) {
		(void)can_io_set_loopback(be_cfg.battery2_iface, true);
	}
	(void)can_io_set_loopback(be_cfg.inverter_iface, true);
	next_100ms = k_uptime_get();
	next_1s = k_uptime_get();
}

void sim_battery_tick(void)
{
	int64_t now = k_uptime_get();

	if (now >= next_100ms) {
		next_100ms += 100;
		send_battery_frames(be_cfg.battery_iface);
		if (be_cfg.battery2_type != 0) {
			send_battery_frames(be_cfg.battery2_iface);
		}
	}
	if (now >= next_1s) {
		const uint8_t f4200[8] = {0};

		next_1s += 1000;
		/* Extended ID, like the Pylon protocol */
		(void)can_io_send_raw(be_cfg.inverter_iface, 0x4200, true, false, 8, f4200);
	}
	/* The consecutive frames of a UDS answer, 10 ms apart like in the capture */
	for (uint8_t i = 0; i < BE_NUM_CAN; i++) {
		struct sim_uds_state *u = &uds[i];

		if (u->pending != NULL && u->next != 0xFF && now >= u->next_cf) {
			u->next_cf = now + 10;
			send8(i, 0x7EC, u->pending->frames[u->next]);
			if (++u->next >= u->pending->n_frames) {
				u->pending = NULL;
			}
		}
	}
}

bool sim_battery_on_frame(const struct be_can_rx *f)
{
	if (f->iface == be_cfg.inverter_iface) {
		if (f->id == 0x4210) {
			memcpy(inverter_421x, f->data, 8);
			inverter_421x_count++;
		}
		return f->id == 0x4200; /* the only frame from the simulated inverter */
	}
	if (f->id != 0x7E4) {
		/* Frames from the simulated battery go to the driver, its own 0x2xx/0x5xx do not */
		switch (f->id) {
		case 0x542: case 0x594: case 0x595: case 0x596: case 0x5D5: case 0x7EC:
			return true;
		default:
			return false;
		}
	}
	struct sim_uds_state *u = &uds[f->iface];

	if (f->data[0] == 0x03 && f->data[1] == 0x22) {
		uint16_t pid = (f->data[2] << 8) | f->data[3];

		u->pending = NULL;
		for (size_t i = 0; i < ARRAY_SIZE(sim_uds_responses); i++) {
			if (sim_uds_responses[i].pid == pid) {
				send8(f->iface, 0x7EC, sim_uds_responses[i].frames[0]);
				u->pending = &sim_uds_responses[i];
				u->next = 0xFF; /* waits for flow control */
				break;
			}
		}
	} else if ((f->data[0] & 0xF0) == 0x30 && u->pending != NULL && u->next == 0xFF) {
		u->next = 1;
	}
	return false;
}

int sim_battery_reg_read(uint16_t addr, uint16_t *v)
{
	/* 400: simulator present; 401-408: last 0x4210 frame to the inverter; 409: count */
	if (addr == 400) {
		*v = 1;
	} else if (addr >= 401 && addr <= 408) {
		*v = inverter_421x[addr - 401];
	} else if (addr == 409) {
		*v = (uint16_t)inverter_421x_count;
	} else {
		*v = 0;
	}
	return 0;
}
