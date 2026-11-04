/*
 * Settings of the upstream code on the S32K, by the same names as Preferences on the ESP32 (BATTCHEM,
 * PYLONSEND, ...), kept in NVS with a copy in RAM. The handful that have a place in be_cfg (battery
 * type, CAN channels, limits) live only there, see be_link_cmd.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BE_KV_H
#define BE_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BE_KV_KEY_LEN 16 /* with the terminator; NVS on the ESP32 allows 15 characters */
#define BE_KV_VAL_LEN 32 /* strings, with the terminator */

/* Same values as enum be_set_vtype in be_snapshot.h */
enum be_kv_type { BE_KV_INT = 1, BE_KV_UINT = 2, BE_KV_BOOL = 3, BE_KV_STR = 4 };

struct be_kv_entry {
	char key[BE_KV_KEY_LEN];
	uint8_t type;
	uint8_t len;
	uint8_t val[BE_KV_VAL_LEN]; /* numbers: uint32_t little endian */
};

bool be_kv_has(const char *key);
bool be_kv_get_u32(const char *key, uint32_t *value);
/* Copies a string value with its terminator, returns its length, or -1 if there is none. */
int be_kv_get_str(const char *key, char *out, size_t cap);

/* Stores in RAM and NVS. The NVS write only if the value changed. */
int be_kv_put(const char *key, uint8_t type, const void *value, size_t len);
int be_kv_put_u32(const char *key, uint8_t type, uint32_t value);
int be_kv_remove(const char *key);
void be_kv_clear(void);

/* Walks the entries, for the sync to the ESP32. NULL past the end. */
const struct be_kv_entry *be_kv_at(int index);
int be_kv_count(void);

/* Bumps on every change. */
uint32_t be_kv_revision(void);

/* From the settings handler of be_cfg.c, for names "k/<key>" at load. */
int be_kv_on_load(const char *key, size_t len, int (*read_cb)(void *arg, void *data, size_t len), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* BE_KV_H */
