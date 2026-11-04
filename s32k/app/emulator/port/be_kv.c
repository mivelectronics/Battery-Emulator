/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "be_kv.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define KV_MAX 128

/* Plain CPU data, so DTCM */
static struct be_kv_entry __dtcm_bss_section table[KV_MAX];
static int count;
static uint32_t revision;
static K_MUTEX_DEFINE(lock);

static struct be_kv_entry *find(const char *key)
{
	for (int i = 0; i < count; i++) {
		if (strncmp(table[i].key, key, BE_KV_KEY_LEN) == 0) {
			return &table[i];
		}
	}
	return NULL;
}

bool be_kv_has(const char *key)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool has = find(key) != NULL;

	k_mutex_unlock(&lock);
	return has;
}

bool be_kv_get_u32(const char *key, uint32_t *value)
{
	k_mutex_lock(&lock, K_FOREVER);
	const struct be_kv_entry *e = find(key);
	bool ok = e != NULL && e->type != BE_KV_STR;

	if (ok) {
		memcpy(value, e->val, sizeof(*value));
	}
	k_mutex_unlock(&lock);
	return ok;
}

int be_kv_get_str(const char *key, char *out, size_t cap)
{
	int n = -1;

	k_mutex_lock(&lock, K_FOREVER);
	const struct be_kv_entry *e = find(key);

	if (e != NULL && e->type == BE_KV_STR && cap > 0) {
		n = MIN((size_t)e->len, cap - 1);
		memcpy(out, e->val, n);
		out[n] = 0;
	}
	k_mutex_unlock(&lock);
	return n;
}

/* NVS entry: type byte, then the value */
static int store(const struct be_kv_entry *e)
{
	char name[8 + BE_KV_KEY_LEN];
	uint8_t blob[1 + BE_KV_VAL_LEN];

	snprintf(name, sizeof(name), "be/k/%s", e->key);
	blob[0] = e->type;
	memcpy(blob + 1, e->val, e->len);
	return settings_save_one(name, blob, 1 + e->len);
}

int be_kv_put(const char *key, uint8_t type, const void *value, size_t len)
{
	if (strlen(key) >= BE_KV_KEY_LEN || len > BE_KV_VAL_LEN || type < BE_KV_INT || type > BE_KV_STR) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	struct be_kv_entry *e = find(key);
	int err = 0;

	if (e != NULL && e->type == type && e->len == len && memcmp(e->val, value, len) == 0) {
		k_mutex_unlock(&lock);
		return 0;
	}
	if (e == NULL) {
		if (count >= KV_MAX) {
			k_mutex_unlock(&lock);
			printk("kv: table full, %s not stored\n", key);
			return -ENOMEM;
		}
		e = &table[count++];
		memset(e, 0, sizeof(*e));
		strncpy(e->key, key, BE_KV_KEY_LEN - 1);
	}
	e->type = type;
	e->len = (uint8_t)len;
	memcpy(e->val, value, len);
	err = store(e);
	revision++;
	k_mutex_unlock(&lock);
	return err;
}

int be_kv_put_u32(const char *key, uint8_t type, uint32_t value)
{
	return be_kv_put(key, type, &value, sizeof(value));
}

int be_kv_remove(const char *key)
{
	char name[8 + BE_KV_KEY_LEN];

	k_mutex_lock(&lock, K_FOREVER);
	struct be_kv_entry *e = find(key);
	int err = 0;

	if (e != NULL) {
		snprintf(name, sizeof(name), "be/k/%s", e->key);
		err = settings_delete(name);
		*e = table[--count];
		revision++;
	}
	k_mutex_unlock(&lock);
	return err;
}

void be_kv_clear(void)
{
	char name[8 + BE_KV_KEY_LEN];

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < count; i++) {
		snprintf(name, sizeof(name), "be/k/%s", table[i].key);
		(void)settings_delete(name);
	}
	count = 0;
	revision++;
	k_mutex_unlock(&lock);
}

const struct be_kv_entry *be_kv_at(int index)
{
	return (index >= 0 && index < count) ? &table[index] : NULL;
}

int be_kv_count(void)
{
	return count;
}

uint32_t be_kv_revision(void)
{
	return revision;
}

int be_kv_on_load(const char *key, size_t len, int (*read_cb)(void *arg, void *data, size_t len), void *arg)
{
	uint8_t blob[1 + BE_KV_VAL_LEN];

	if (len < 1 || len > sizeof(blob) || strlen(key) >= BE_KV_KEY_LEN) {
		return -EINVAL;
	}
	int n = read_cb(arg, blob, len);

	if (n != (int)len) {
		return -EIO;
	}
	k_mutex_lock(&lock, K_FOREVER);
	struct be_kv_entry *e = find(key);

	if (e == NULL) {
		if (count >= KV_MAX) {
			k_mutex_unlock(&lock);
			return -ENOMEM;
		}
		e = &table[count++];
		memset(e, 0, sizeof(*e));
		strncpy(e->key, key, BE_KV_KEY_LEN - 1);
	}
	e->type = blob[0];
	e->len = (uint8_t)(len - 1);
	memcpy(e->val, blob + 1, len - 1);
	k_mutex_unlock(&lock);
	return 0;
}
