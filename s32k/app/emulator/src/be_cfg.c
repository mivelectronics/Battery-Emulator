/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_cfg.h"
#include "be_kv.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <fsl_power.h>

struct be_cfg be_cfg;

static struct be_cfg loaded;
static bool loaded_valid;

void be_cfg_defaults(struct be_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->version = BE_CFG_VERSION;
	/* CAN0: inverter, classic CAN 500 kbit/s. CAN1/CAN4: batteries, CAN FD 500k/2M */
	cfg->can[0] = (struct be_cfg_can){.nominal_kbps = 500, .data_kbps = 0, .enabled = 1};
	cfg->can[1] = (struct be_cfg_can){.nominal_kbps = 500, .data_kbps = 2000, .enabled = 1};
	cfg->can[2] = (struct be_cfg_can){.nominal_kbps = 500, .data_kbps = 2000, .enabled = 1};
	cfg->mb_unit_id = 1;
	cfg->mb_parity = 2; /* Modbus RTU default: 8E1 */
	cfg->mb_baud_div100 = 192;
	cfg->battery_iface = 1;
	cfg->battery2_iface = 2;
	cfg->inverter_iface = 0;
	cfg->max_percentage = 8000;
	cfg->min_percentage = 2000;
	cfg->max_charge_dA = 300;
	cfg->max_discharge_dA = 300;
	cfg->precharge_ms = 100;
#ifdef BE_SIM
	/* Bench simulator: starts as a Kia/Hyundai 64 battery with a Pylon inverter */
	cfg->battery_type = 17;
	cfg->inverter_type = 10;
#endif
}

int be_cfg_validate(const struct be_cfg *cfg)
{
	static const uint16_t nominal_ok[] = {100, 125, 200, 250, 500, 800, 1000};

	for (int i = 0; i < BE_NUM_CAN; i++) {
		const struct be_cfg_can *c = &cfg->can[i];
		bool ok = false;

		for (size_t k = 0; k < ARRAY_SIZE(nominal_ok); k++) {
			ok |= c->nominal_kbps == nominal_ok[k];
		}
		if (!ok || c->enabled > 1) {
			return -EINVAL;
		}
		if (c->data_kbps != 0 && (c->data_kbps < c->nominal_kbps || c->data_kbps > 5000)) {
			return -EINVAL;
		}
	}
	if (cfg->mb_unit_id < 1 || cfg->mb_unit_id > 247 || cfg->mb_parity > 2) {
		return -EINVAL;
	}
	if (cfg->battery_iface >= BE_NUM_CAN || cfg->battery2_iface >= BE_NUM_CAN ||
	    cfg->inverter_iface >= BE_NUM_CAN || cfg->soc_scaling > 1) {
		return -EINVAL;
	}
	if (cfg->battery_type != 0 && cfg->battery_iface == cfg->inverter_iface) {
		return -EINVAL;
	}
	if (cfg->battery2_type != 0 && (cfg->battery2_iface == cfg->battery_iface ||
					cfg->battery2_iface == cfg->inverter_iface)) {
		return -EINVAL;
	}
	if (cfg->max_percentage > 10000 || cfg->min_percentage >= (int32_t)cfg->max_percentage) {
		return -EINVAL;
	}
	/* Closing without a real precharge time would be the inrush the precharge is there to prevent */
	if (cfg->contactor_ctrl > 1 || cfg->contactor2_ctrl > 1 || cfg->contactor_nc > 1 ||
	    cfg->precharge_ms < 10 || cfg->precharge_ms > 60000) {
		return -EINVAL;
	}
	switch (cfg->mb_baud_div100) {
	case 96: case 192: case 384: case 576: case 1152:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int be_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strncmp(name, "k/", 2) == 0) {
		return be_kv_on_load(name + 2, len, (int (*)(void *, void *, size_t))read_cb, cb_arg);
	}
	if (strcmp(name, "cfg") != 0) {
		return -ENOENT;
	}
	struct be_cfg tmp;

	be_cfg_defaults(&tmp);
	/* Older/newer layouts: take the common prefix, the rest stays at the defaults */
	ssize_t n = read_cb(cb_arg, &tmp, MIN(len, sizeof(tmp)));

	if (n < (ssize_t)sizeof(uint16_t)) {
		return -EINVAL;
	}
	tmp.version = BE_CFG_VERSION;
	if (be_cfg_validate(&tmp) == 0) {
		loaded = tmp;
		loaded_valid = true;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(be, "be", NULL, be_settings_set, NULL, NULL);

int be_cfg_reload(struct be_cfg *cfg)
{
	loaded_valid = false;
	int err = settings_load_subtree("be");

	if (err == 0 && loaded_valid) {
		*cfg = loaded;
		return 0;
	}
	be_cfg_defaults(cfg);
	return err ? err : -ENOENT;
}

int be_cfg_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		printk("settings init failed: %d, using defaults\n", err);
		be_cfg_defaults(&be_cfg);
		return err;
	}
	err = be_cfg_reload(&be_cfg);
	printk("config: %s, revision %u\n", err ? "defaults" : "loaded from NVS", be_cfg.revision);
	return 0;
}

void be_cfg_system_reset(void)
{
	/*
	 * The request is a two-word key sequence to MC_ME. An interrupt between the two words cancels it
	 * and POWER_RequestResetMode() then waits forever: about 1 reset in 5 with CAN traffic running.
	 *
	 * Functional, not destructive: measured on FRDM-A-S32K312, the 8th destructive reset after a
	 * flash load left the chip dead before the first line of the boot, with the escalation
	 * thresholds at 0 and the flash intact. 29 functional resets in a row were fine. Cause not known.
	 */
	(void)irq_lock();
	POWER_RequestResetMode(kMC_ME_FunctionalReset);
	while (1) {
		k_cpu_idle();
	}
}

static void reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	be_cfg_system_reset();
}
static K_WORK_DELAYABLE_DEFINE(reset_work, reset_work_handler);

void be_cfg_system_reset_in(uint32_t ms)
{
	k_work_schedule(&reset_work, K_MSEC(ms));
}

int be_cfg_save(const struct be_cfg *cfg)
{
	struct be_cfg tmp = *cfg;
	int err = be_cfg_validate(&tmp);

	if (err) {
		return err;
	}
	tmp.version = BE_CFG_VERSION;
	tmp.revision = be_cfg.revision + 1;
	err = settings_save_one("be/cfg", &tmp, sizeof(tmp));
	if (err) {
		return err;
	}
	be_cfg = tmp;
	return 0;
}
