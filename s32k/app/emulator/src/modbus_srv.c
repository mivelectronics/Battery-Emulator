/*
 * Modbus RTU server. Holding registers (FC03 read, FC06/FC16 write); the same addresses can be
 * read as input registers (FC04). 32-bit values: high word first. See MODBUS_MAP.md.
 *
 * Settings are edited in a staging copy: unlock with 0xA5C3 in register 5000, write the setting
 * registers, then command 1 in register 5001 validates, stores to NVS and applies them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "modbus_srv.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/printk.h>

#include "be_cfg.h"
#include "can_io.h"
#include "link.h"

#define FW_VERSION_REG 0x0002 /* major << 8 | minor */

#define REG_SYS_BASE  0
#define REG_CAN_BASE  100
#define REG_CAN_STEP  20
#define REG_CFG_BASE  5000

#define UNLOCK_KEY    0xA5C3
#define UNLOCK_MS     60000

enum mb_cmd {
	CMD_SAVE_APPLY = 1,
	CMD_RELOAD = 2,
	CMD_DEFAULTS = 3,
	CMD_REBOOT = 0xB007,
};

static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	be_cfg_system_reset();
}
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_handler);

static struct be_cfg stage;     /* settings being edited */
static int64_t unlocked_until;  /* uptime ms */
static int16_t last_cmd_result; /* 0 ok, negative errno */

static bool unlocked(void)
{
	return k_uptime_get() < unlocked_until;
}

static uint16_t hi16(uint32_t v)
{
	return (uint16_t)(v >> 16);
}

static uint16_t lo16(uint32_t v)
{
	return (uint16_t)v;
}

/* ---- read ---------------------------------------------------------------------------------- */

static int read_sys(uint16_t r, uint16_t *v)
{
	uint32_t up = (uint32_t)(k_uptime_get() / 1000);

	switch (r) {
	case 0: *v = MB_MAP_VERSION; break;
	case 1: *v = FW_VERSION_REG; break;
	case 2: *v = hi16(up); break;
	case 3: *v = lo16(up); break;
	case 4: *v = hi16(be_cfg.revision); break;
	case 5: *v = lo16(be_cfg.revision); break;
	case 6: *v = link_peer_alive() ? 1 : 0; break;
	case 7: *v = unlocked() ? 1 : 0; break;
	case 8: *v = (uint16_t)last_cmd_result; break;
	case 9: *v = (memcmp(&stage, &be_cfg, sizeof(stage)) != 0) ? 1 : 0; break; /* unsaved edits */
	default: *v = 0; break; /* reserved: read as 0 */
	}
	return 0;
}

static int read_can(uint8_t i, uint16_t r, uint16_t *v)
{
	const struct can_io_stats *s = &can_io_stats[i];
	enum can_state st = CAN_STATE_STOPPED;
	struct can_bus_err_cnt ec = {0};

	if (r <= 2) {
		(void)can_io_state(i, &st, &ec);
	}
	switch (r) {
	case 0: *v = (uint16_t)st; break;
	case 1: *v = ec.tx_err_cnt; break;
	case 2: *v = ec.rx_err_cnt; break;
	case 3: *v = hi16(s->rx); break;
	case 4: *v = lo16(s->rx); break;
	case 5: *v = hi16(s->tx_ok); break;
	case 6: *v = lo16(s->tx_ok); break;
	case 7: *v = hi16(s->tx_err); break;
	case 8: *v = lo16(s->tx_err); break;
	case 9: *v = s->nominal_kbps; break;
	case 10: *v = s->data_kbps; break;
	default: *v = 0; break;
	}
	return 0;
}

/* Setting registers 5010.. map onto fields of a struct be_cfg; returns a pointer or NULL */
static uint16_t *cfg_reg16(struct be_cfg *c, uint16_t off, bool *is_u8)
{
	static uint16_t tmp8;

	*is_u8 = false;
	if (off >= 10 && off < 10 + 4 * BE_NUM_CAN) {
		uint8_t i = (off - 10) / 4;

		switch ((off - 10) % 4) {
		case 0: return &c->can[i].nominal_kbps;
		case 1: return &c->can[i].data_kbps;
		case 2: *is_u8 = true; tmp8 = c->can[i].enabled; return &tmp8;
		default: return NULL;
		}
	}
	switch (off) {
	case 30: *is_u8 = true; tmp8 = c->mb_unit_id; return &tmp8;
	case 31: return &c->mb_baud_div100;
	case 32: *is_u8 = true; tmp8 = c->mb_parity; return &tmp8;
	case 40: return &c->battery_type;
	case 41: return &c->battery2_type;
	case 42: return &c->inverter_type;
	default: return NULL;
	}
}

static int read_cfg(uint16_t off, uint16_t *v)
{
	bool is_u8;

	if (off == 0) {
		*v = unlocked() ? 1 : 0;
		return 0;
	}
	if (off == 1) {
		*v = (uint16_t)last_cmd_result;
		return 0;
	}
	uint16_t *p = cfg_reg16(&stage, off, &is_u8);

	*v = p ? *p : 0;
	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr < REG_CAN_BASE) {
		return read_sys(addr - REG_SYS_BASE, reg);
	}
	if (addr < REG_CAN_BASE + REG_CAN_STEP * BE_NUM_CAN) {
		return read_can((addr - REG_CAN_BASE) / REG_CAN_STEP, (addr - REG_CAN_BASE) % REG_CAN_STEP,
				reg);
	}
	if (addr >= REG_CFG_BASE && addr < REG_CFG_BASE + 100) {
		return read_cfg(addr - REG_CFG_BASE, reg);
	}
	return -ENOTSUP;
}

/* ---- write --------------------------------------------------------------------------------- */

static void apply_cfg(void)
{
	for (uint8_t i = 0; i < BE_NUM_CAN; i++) {
		(void)can_io_apply(i);
	}
}

static int do_command(uint16_t cmd)
{
	int err = 0;

	switch (cmd) {
	case CMD_SAVE_APPLY:
		err = be_cfg_save(&stage);
		if (err == 0) {
			stage = be_cfg;
			apply_cfg();
		}
		break;
	case CMD_RELOAD:
		(void)be_cfg_reload(&stage);
		break;
	case CMD_DEFAULTS:
		be_cfg_defaults(&stage);
		stage.revision = be_cfg.revision;
		break;
	case CMD_REBOOT:
		/* Later, so that this write's response still goes out */
		k_work_schedule(&reboot_work, K_MSEC(200));
		break;
	default:
		err = -EINVAL;
		break;
	}
	last_cmd_result = (int16_t)err;
	printk("modbus: command %u -> %d\n", cmd, err);
	return 0; /* the result is reported in register 5001, not as an exception */
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	if (addr == REG_CFG_BASE) {
		if (reg == UNLOCK_KEY) {
			unlocked_until = k_uptime_get() + UNLOCK_MS;
		} else {
			unlocked_until = 0;
		}
		return 0;
	}
	if (!unlocked()) {
		return -EACCES;
	}
	unlocked_until = k_uptime_get() + UNLOCK_MS; /* every write extends the window */
	if (addr == REG_CFG_BASE + 1) {
		return do_command(reg);
	}
	if (addr < REG_CFG_BASE || addr >= REG_CFG_BASE + 100) {
		return -ENOTSUP;
	}
	bool is_u8;
	uint16_t off = addr - REG_CFG_BASE;
	uint16_t *p = cfg_reg16(&stage, off, &is_u8);

	if (p == NULL) {
		return -ENOTSUP;
	}
	if (!is_u8) {
		*p = reg;
		return 0;
	}
	if (reg > 0xFF) {
		return -EINVAL;
	}
	if (off >= 10 && off < 10 + 4 * BE_NUM_CAN) {
		stage.can[(off - 10) / 4].enabled = (uint8_t)reg;
	} else if (off == 30) {
		stage.mb_unit_id = (uint8_t)reg;
	} else if (off == 32) {
		stage.mb_parity = (uint8_t)reg;
	}
	return 0;
}

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	return holding_reg_rd(addr, reg);
}

static struct modbus_user_callbacks cbs = {
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
	.input_reg_rd = input_reg_rd,
};

int modbus_srv_init(void)
{
	static const enum uart_config_parity parity[] = {
		UART_CFG_PARITY_NONE, UART_CFG_PARITY_ODD, UART_CFG_PARITY_EVEN};
	int iface = modbus_iface_get_by_name("modbus0");

	stage = be_cfg;
	if (iface < 0) {
		printk("modbus: no modbus0 interface\n");
		return iface;
	}
	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RTU,
		.server = {.user_cb = &cbs, .unit_id = be_cfg.mb_unit_id},
		.serial = {
			.baud = be_cfg.mb_baud_div100 * 100U,
			.parity = parity[be_cfg.mb_parity],
			/* Modbus RTU: 2 stop bits without parity, 1 with parity */
			.stop_bits = be_cfg.mb_parity ? UART_CFG_STOP_BITS_1 : UART_CFG_STOP_BITS_2,
		},
	};
	int err = modbus_init_server(iface, param);

	printk("modbus: RTU server unit %u, %u baud, parity %u: %d\n", be_cfg.mb_unit_id,
	       param.serial.baud, be_cfg.mb_parity, err);
	return err;
}
