/*
 * Preferences for the Battery-Emulator core on the S32K. Upstream reads every setting through this
 * class with its own default value, so comm_nvm.cpp runs unchanged and gives the same defaults as on
 * the ESP32. Nothing is stored here yet: settings the S32K owns come from be_cfg (NVS) and are applied
 * on top (be_core.cpp). Next step: a key/value backend in NVS.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PREFERENCES
#define PREFERENCES

#include <WString.h>
#include <stdint.h>
#include <string.h>

class Preferences {
 public:
  Preferences() {}

  bool begin(const char* name, bool readOnly = false, const char* partition_label = NULL) { return true; }
  void end() {}
  bool clear() { return true; }

  size_t putInt(const char* key, int32_t value) { return 0; }
  size_t putUInt(const char* key, uint32_t value) { return 0; }
  size_t putBool(const char* key, bool value) { return 0; }
  size_t putString(const char* key, const char* value) { return 0; }
  size_t putString(const char* key, String value) { return 0; }
  size_t putBytes(const char* key, const void* value, size_t len) { return 0; }

  bool isKey(const char* key) { return false; }
  bool remove(const char* key) { return true; }

  int32_t getInt(const char* key, int32_t defaultValue = 0) { return defaultValue; }
  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) { return defaultValue; }
  bool getBool(const char* key, bool defaultValue = false) { return defaultValue; }
  size_t getString(const char* key, char* value, size_t maxLen) { return 0; }
  String getString(const char* key, String defaultValue = String()) { return defaultValue; }
  size_t getBytes(const char* key, void* buf, size_t maxLen) { return 0; }
  size_t getBytesLength(const char* key) { return 0; }
};
#endif
