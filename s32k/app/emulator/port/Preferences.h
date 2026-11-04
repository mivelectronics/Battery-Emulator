/*
 * Preferences for the Battery-Emulator core on the S32K, on top of be_kv (NVS). Upstream reads every
 * setting through this class with its own default value, so comm_nvm.cpp runs unchanged and gives the
 * same defaults as on the ESP32.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PREFERENCES
#define PREFERENCES

#include <WString.h>
#include <stdint.h>
#include <string.h>

#include "be_kv.h"

class Preferences {
 public:
  Preferences() {}

  bool begin(const char* name, bool readOnly = false, const char* partition_label = NULL) { return true; }
  void end() {}
  bool clear() {
    be_kv_clear();
    return true;
  }

  size_t putInt(const char* key, int32_t value) {
    return be_kv_put_u32(key, BE_KV_INT, (uint32_t)value) == 0 ? sizeof(value) : 0;
  }
  size_t putUInt(const char* key, uint32_t value) {
    return be_kv_put_u32(key, BE_KV_UINT, value) == 0 ? sizeof(value) : 0;
  }
  size_t putBool(const char* key, bool value) { return be_kv_put_u32(key, BE_KV_BOOL, value ? 1 : 0) == 0 ? 1 : 0; }
  size_t putString(const char* key, const char* value) {
    const size_t n = strlen(value);
    return be_kv_put(key, BE_KV_STR, value, n) == 0 ? n : 0;
  }
  size_t putString(const char* key, String value) { return putString(key, value.c_str()); }
  size_t putBytes(const char* key, const void* value, size_t len) { return 0; }

  bool isKey(const char* key) { return be_kv_has(key); }
  bool remove(const char* key) { return be_kv_remove(key) == 0; }

  int32_t getInt(const char* key, int32_t defaultValue = 0) {
    uint32_t v;
    return be_kv_get_u32(key, &v) ? (int32_t)v : defaultValue;
  }
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    uint32_t v;
    return be_kv_get_u32(key, &v) ? v : defaultValue;
  }
  bool getBool(const char* key, bool defaultValue = false) {
    uint32_t v;
    return be_kv_get_u32(key, &v) ? v != 0 : defaultValue;
  }
  size_t getString(const char* key, char* value, size_t maxLen) {
    const int n = be_kv_get_str(key, value, maxLen);
    return n < 0 ? 0 : (size_t)n;
  }
  String getString(const char* key, String defaultValue = String()) {
    char buf[BE_KV_VAL_LEN + 1];
    return be_kv_get_str(key, buf, sizeof(buf)) < 0 ? defaultValue : String(buf);
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) { return 0; }
  size_t getBytesLength(const char* key) { return 0; }
};
#endif
