/*
 * What the ESP32 asks of the S32K over the link: commands and settings. Runs in the main thread, which
 * is also the one that may wait for flash. Things that touch the drivers go to the core thread.
 *
 * Settings with a place in be_cfg are collected and validated together after a short quiet time, since
 * a form arrives one key at a time and may pass through invalid combinations on the way.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_link_cmd.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "be_cfg.h"
#include "be_core.h"
#include "be_kv.h"
#include "be_ota.h"
#include "be_snapshot.h"
#include "link.h"

#define COMMIT_QUIET_MS 500
#define SYNC_FRAMES_PER_POLL 8

static uint16_t last_id;
static bool last_id_valid;

K_MSGQ_DEFINE(core_cmd_q, sizeof(struct be_link_cmd), 8, 4);

/* Live battery_settings on their way to the core thread */
static uint8_t live_settings[BE_SNAPSHOT_SETTINGS_MAX];
static uint16_t live_settings_len;
static bool live_settings_pending;

static struct be_cfg pending_cfg;
static bool pending_cfg_dirty;
static int64_t pending_cfg_since;

static int sync_index = -1; /* -1 idle, then be_kv entries, then the be_cfg ones */

static void log_to_net(const char *text)
{
	printk("%s\n", text);
	(void)link_send(BE_LINK_LOG, text, (uint16_t)strlen(text));
}

/* comm_interface of upstream to our channel number */
static int iface_of_comm(uint32_t comm)
{
	switch (comm) {
	case 3: /* CanNative */
		return 0;
	case 4: /* CanFdNative */
	case 6: /* CanFdAddonMcp2518 */
		return 1;
	case 7: /* CanFdAddonMcp2518_2 */
		return 2;
	default:
		return -1;
	}
}

static uint32_t comm_of_iface(uint8_t iface)
{
	static const uint8_t comm[BE_NUM_CAN] = {3, 6, 7};

	return comm[iface < BE_NUM_CAN ? iface : 0];
}

/* Returns 1 if the key belongs to be_cfg, 0 if not, negative if the value cannot be used. */
static int map_to_cfg(const char *key, uint32_t v)
{
	struct be_cfg *c = &pending_cfg;
	int iface;

	if (strcmp(key, "BATTTYPE") == 0) {
		c->battery_type = (uint16_t)v;
	} else if (strcmp(key, "INVTYPE") == 0) {
		c->inverter_type = (uint16_t)v;
	} else if (strcmp(key, "DBLBTR") == 0) {
		c->battery2_type = v ? 1 : 0;
	} else if (strcmp(key, "BATTCOMM") == 0 || strcmp(key, "BATT2COMM") == 0 ||
		   strcmp(key, "INVCOMM") == 0) {
		iface = iface_of_comm(v);
		if (iface < 0) {
			return -EINVAL;
		}
		if (key[0] == 'I') {
			c->inverter_iface = (uint8_t)iface;
		} else if (key[4] == '2') {
			c->battery2_iface = (uint8_t)iface;
		} else {
			c->battery_iface = (uint8_t)iface;
		}
	} else if (strcmp(key, "MAXPERCENTAGE") == 0) {
		c->max_percentage = (uint16_t)(v * 10);
	} else if (strcmp(key, "MINPERCENTAGE") == 0) {
		c->min_percentage = (int16_t)((int32_t)v * 10);
	} else if (strcmp(key, "MAXCHARGEAMP") == 0) {
		c->max_charge_dA = (uint16_t)v;
	} else if (strcmp(key, "MAXDISCHARGEAMP") == 0) {
		c->max_discharge_dA = (uint16_t)v;
	} else if (strcmp(key, "USE_SCALED_SOC") == 0) {
		c->soc_scaling = v ? 1 : 0;
	} else if (strcmp(key, "CNTCTRL") == 0) {
		c->contactor_ctrl = v ? 1 : 0;
	} else if (strcmp(key, "CNTCTRLDBL") == 0) {
		c->contactor2_ctrl = v ? 1 : 0;
	} else if (strcmp(key, "NCCONTACTOR") == 0) {
		c->contactor_nc = v ? 1 : 0;
	} else if (strcmp(key, "PRECHGMS") == 0) {
		c->precharge_ms = (uint16_t)MIN(v, 0xFFFF);
	} else {
		return 0;
	}
	return 1;
}

static void commit_pending_cfg(void)
{
	if (!pending_cfg_dirty) {
		return;
	}
	pending_cfg_dirty = false;
	int err = memcmp(&pending_cfg, &be_cfg, sizeof(pending_cfg)) == 0 ? 0 : be_cfg_save(&pending_cfg);

	if (err) {
		/* The ESP32 hears the truth in the next sync */
		log_to_net("settings from the web refused: invalid combination (CAN channels or SOC limits)");
		be_link_cmd_request_sync();
		return;
	}
	be_core_request_reload();
}

static void handle_kv(const struct be_set_kv *m, uint16_t value_len)
{
	char key[BE_KV_KEY_LEN];

	memcpy(key, m->key, sizeof(key));
	key[sizeof(key) - 1] = 0;

	if (m->hdr.vtype != BE_KV_STR && value_len == sizeof(uint32_t)) {
		uint32_t v;

		memcpy(&v, m->value, sizeof(v));
		if (v != 0 && (strcmp(key, "PWMCNTCTRL") == 0 || strcmp(key, "CNTCTRLTRI") == 0)) {
			/* A setting the outputs here cannot do must not look as if it were in force */
			log_to_net("setting refused: no PWM hold and no third battery contactor on the S32K");
			be_link_cmd_request_sync();
			return;
		}
		if (!pending_cfg_dirty) {
			pending_cfg = be_cfg;
		}
		int mapped = map_to_cfg(key, v);

		if (mapped != 0) {
			pending_cfg_dirty = true;
			pending_cfg_since = k_uptime_get();
			if (mapped < 0) {
				log_to_net("setting refused: this channel is not on the S32K");
			}
			return;
		}
	}
	(void)be_kv_put(key, m->hdr.vtype, m->value, value_len);
	be_core_request_reload();
}

static int handle_settings(const struct be_link_frame *f)
{
	struct be_set_hdr h;

	memcpy(&h, f->payload, sizeof(h));
	switch (h.kind) {
	case BE_SET_LIVE:
		if (f->len - sizeof(h) > sizeof(live_settings)) {
			return -EINVAL;
		}
		unsigned int irq = irq_lock();

		memcpy(live_settings, f->payload + sizeof(h), f->len - sizeof(h));
		live_settings_len = f->len - sizeof(h);
		live_settings_pending = true;
		irq_unlock(irq);
		return 0;
	case BE_SET_KV: {
		static struct be_set_kv m;

		if (f->len < offsetof(struct be_set_kv, value) || f->len > sizeof(m)) {
			return -EINVAL;
		}
		memset(&m, 0, sizeof(m));
		memcpy(&m, f->payload, f->len);
		handle_kv(&m, f->len - offsetof(struct be_set_kv, value));
		return 0;
	}
	case BE_SET_KV_REMOVE: {
		struct be_set_kv m = {0};

		memcpy(&m, f->payload, MIN(f->len, sizeof(m)));
		m.key[sizeof(m.key) - 1] = 0;
		(void)be_kv_remove(m.key);
		be_core_request_reload();
		return 0;
	}
	case BE_SET_CLEAR_ALL: {
		be_kv_clear();
		struct be_cfg def;

		be_cfg_defaults(&def);
		pending_cfg_dirty = false;
		return be_cfg_save(&def);
	}
	default:
		return -EINVAL;
	}
}

static int handle_cmd(const struct be_link_frame *f)
{
	struct be_link_cmd c = {0};

	memcpy(&c, f->payload, MIN(f->len, sizeof(c)));
	if (c.code == BE_CMD_SETTINGS_SYNC) {
		be_link_cmd_request_sync();
		return 0;
	}
	if (c.code == BE_CMD_RESTART) {
		commit_pending_cfg(); /* the form just before it must not get lost */
	}
	return k_msgq_put(&core_cmd_q, &c, K_NO_WAIT) == 0 ? 0 : -EBUSY;
}

bool be_link_cmd_frame(const struct be_link_frame *f)
{
	bool is_cmd = f->type == BE_LINK_CMD;

	if (!is_cmd && f->type != BE_LINK_SETTINGS) {
		return false;
	}
	if (f->len < sizeof(struct be_set_hdr)) {
		return true;
	}
	/* Both layouts have the 16-bit id at offset 2 */
	uint16_t id;

	memcpy(&id, f->payload + 2, sizeof(id));
	if (id == 0 || (is_cmd && ((const struct be_link_cmd *)f->payload)->code < BE_CMD_FIRST)) {
		return false; /* the older bridge commands, handled by the caller */
	}
	int status = 0;

	if (!last_id_valid || id != last_id) {
		status = !be_core_active() ? -EPERM : (is_cmd ? handle_cmd(f) : handle_settings(f));
		last_id = id;
		last_id_valid = true;
	}
	struct be_link_ack a = {.acked_type = f->type, .acked_seq = (uint8_t)id, .status = (int16_t)status};

	(void)link_send(BE_LINK_ACK, &a, sizeof(a));
	return true;
}

bool be_link_cmd_pop_core(struct be_link_cmd *c)
{
	return k_msgq_get(&core_cmd_q, c, K_NO_WAIT) == 0;
}

uint16_t be_link_cmd_pop_live(void *out, uint16_t cap)
{
	uint16_t n = 0;
	unsigned int irq = irq_lock();

	if (live_settings_pending) {
		live_settings_pending = false;
		n = MIN(live_settings_len, cap);
		memcpy(out, live_settings, n);
	}
	irq_unlock(irq);
	return n;
}

void be_link_cmd_request_sync(void)
{
	sync_index = 0;
}

static void send_sync_u32(const char *key, uint8_t type, uint32_t value)
{
	struct be_set_kv m = {.hdr = {.kind = BE_SET_SYNC_KV, .vtype = type}};

	strncpy(m.key, key, sizeof(m.key) - 1);
	memcpy(m.value, &value, sizeof(value));
	(void)link_send(BE_LINK_SETTINGS, &m, offsetof(struct be_set_kv, value) + sizeof(value));
}

/* The be_cfg settings by their upstream names. Returns false past the end. */
static bool send_sync_cfg(int n)
{
	const struct be_cfg *c = &be_cfg;

	switch (n) {
	case 0: send_sync_u32("BATTTYPE", BE_KV_UINT, c->battery_type); break;
	case 1: send_sync_u32("INVTYPE", BE_KV_UINT, c->inverter_type); break;
	case 2: send_sync_u32("DBLBTR", BE_KV_BOOL, c->battery2_type != 0); break;
	case 3: send_sync_u32("BATTCOMM", BE_KV_UINT, comm_of_iface(c->battery_iface)); break;
	case 4: send_sync_u32("BATT2COMM", BE_KV_UINT, comm_of_iface(c->battery2_iface)); break;
	case 5: send_sync_u32("INVCOMM", BE_KV_UINT, comm_of_iface(c->inverter_iface)); break;
	case 6: send_sync_u32("MAXPERCENTAGE", BE_KV_UINT, c->max_percentage / 10); break;
	case 7: send_sync_u32("MINPERCENTAGE", BE_KV_INT, (uint32_t)(int32_t)(c->min_percentage / 10)); break;
	case 8: send_sync_u32("MAXCHARGEAMP", BE_KV_UINT, c->max_charge_dA); break;
	case 9: send_sync_u32("MAXDISCHARGEAMP", BE_KV_UINT, c->max_discharge_dA); break;
	case 10: send_sync_u32("USE_SCALED_SOC", BE_KV_BOOL, c->soc_scaling); break;
	case 11: send_sync_u32("CNTCTRL", BE_KV_BOOL, c->contactor_ctrl); break;
	case 12: send_sync_u32("CNTCTRLDBL", BE_KV_BOOL, c->contactor2_ctrl); break;
	case 13: send_sync_u32("NCCONTACTOR", BE_KV_BOOL, c->contactor_nc); break;
	case 14: send_sync_u32("PRECHGMS", BE_KV_UINT, c->precharge_ms); break;
	default: return false;
	}
	return true;
}

void be_link_cmd_poll(void)
{
	if (be_ota_busy()) {
		return; /* the link stays quiet during an update */
	}
	if (pending_cfg_dirty && k_uptime_get() - pending_cfg_since >= COMMIT_QUIET_MS) {
		commit_pending_cfg();
	}
	for (int n = 0; sync_index >= 0 && n < SYNC_FRAMES_PER_POLL; n++) {
		const struct be_kv_entry *e = be_kv_at(sync_index);

		if (e != NULL) {
			static struct be_set_kv m;

			memset(&m, 0, sizeof(m));
			m.hdr.kind = BE_SET_SYNC_KV;
			m.hdr.vtype = e->type;
			memcpy(m.key, e->key, sizeof(m.key));
			memcpy(m.value, e->val, e->len);
			(void)link_send(BE_LINK_SETTINGS, &m, offsetof(struct be_set_kv, value) + e->len);
			sync_index++;
		} else if (send_sync_cfg(sync_index - be_kv_count())) {
			sync_index++;
		} else {
			struct be_set_hdr end = {.kind = BE_SET_SYNC_END};

			(void)link_send(BE_LINK_SETTINGS, &end, sizeof(end));
			sync_index = -1;
		}
	}
}

uint32_t be_settings_revision(void)
{
	return be_cfg.revision + be_kv_revision();
}
