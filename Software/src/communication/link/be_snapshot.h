#ifndef BE_SNAPSHOT_H
#define BE_SNAPSHOT_H

/*
 * The live state of the emulator, from the CAN processor (S32K, where the drivers run) to the network
 * processor (ESP32, web UI and MQTT), once a second over the link.
 *
 * The ESP32 stays the same code as upstream as far as the web pages and MQTT are concerned: they read
 * its datalayer and its battery and inverter objects. In this mode ("remote") the datalayer is filled
 * from these frames, stand-in objects take the place of the drivers, and the local core logic is off.
 *
 * The other way, ESP32 to S32K: buttons become commands (the stand-in objects forward what the drivers
 * would have done), a change of the live battery_settings is sent as a whole, and every setting saved on
 * the ESP32 except the network ones is forwarded by name. The S32K holds the truth and sends its settings
 * back whenever they change, so the settings page always shows what the S32K runs with.
 *
 * Which mode: the HELLO of the S32K says whether its core runs (be_link_hello.reserved bit 0).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BE_HELLO_FLAG_CORE_ACTIVE 0x01u

/* BE_LINK_CMD codes. The be_link_cmd.reserved field carries the id of the command. */
enum be_cmd_code {
  BE_CMD_FIRST = 10,
  BE_CMD_BATTERY_ACTION = 10, /* arg0 pack 1..3, arg1 enum be_battery_action */
  BE_CMD_PAUSE = 11,          /* arg0 bit 0 pause battery, bit 1 pause CAN; arg1 EquipmentStop */
  BE_CMD_RESTART = 12,
  BE_CMD_BMS_RESET = 13,
  BE_CMD_CLEAR_EVENTS = 14,
  BE_CMD_SETTINGS_SYNC = 15, /* send all settings back */
};

/* The commands of class Battery the web pages and MQTT use */
enum be_battery_action {
  BE_ACT_ISOLATION_TEST,
  BE_ACT_CLEAR_ISOLATION,
  BE_ACT_CALIBRATE_SOC,
  BE_ACT_RESET_BMS,
  BE_ACT_RESET_SOC,
  BE_ACT_RESET_CRASH,
  BE_ACT_RESET_CONTACTOR,
  BE_ACT_RESET_NVROL,
  BE_ACT_RESET_DTC,
  BE_ACT_READ_DTC,
  BE_ACT_RESET_BECM,
  BE_ACT_OPEN_CONTACTORS,
  BE_ACT_CLOSE_CONTACTORS,
  BE_ACT_TOGGLE_SOC_METHOD,
  BE_ACT_RESET_ENERGY_SAVING,
  BE_ACT_FACTORY_MODE,
  BE_ACT_START_BALANCING,
  BE_ACT_END_BALANCING,
};

/* BE_LINK_SETTINGS */
enum be_set_kind {
  BE_SET_LIVE = 1,      /* NET -> CAN: datalayer.battery_settings as a whole, effective at once */
  BE_SET_KV = 2,        /* NET -> CAN: one stored setting */
  BE_SET_KV_REMOVE = 3, /* NET -> CAN */
  BE_SET_CLEAR_ALL = 4, /* NET -> CAN: factory reset */
  BE_SET_SYNC_KV = 5,   /* CAN -> NET: one setting the S32K holds */
  BE_SET_SYNC_END = 6,  /* CAN -> NET: that was all of them */
};

/* Same id position as be_link_cmd, so that the receiver can read it from either */
struct be_set_hdr {
  uint8_t kind;
  uint8_t vtype; /* 1 int32, 2 uint32, 3 bool, 4 string */
  uint16_t id;
} __attribute__((packed));

#define BE_SET_KEY_LEN 16
#define BE_SET_VALUE_LEN 32
struct be_set_kv {
  struct be_set_hdr hdr;
  char key[BE_SET_KEY_LEN];
  uint8_t value[BE_SET_VALUE_LEN]; /* only as many bytes as the value has are sent */
} __attribute__((packed));

#define BE_SNAPSHOT_SETTINGS_MAX 96u

/* Hash of the wire layout: HELLO carries it, and the ESP32 refuses a snapshot from a different build. */
uint32_t be_snapshot_layout_hash(void);

#ifdef CAN_VIA_LINK
/* ESP32, any task: queued for the core task, which sends and repeats until the S32K acknowledges */
bool be_remote_cmd(uint16_t code, uint32_t arg0, uint32_t arg1);
/* True if the setting went to the S32K or does not concern it. From BatteryEmulatorSettingsStore. */
void be_remote_forward_setting(const char* key, uint8_t vtype, const void* value, uint16_t len);
void be_remote_forward_remove(const char* key);
void be_remote_forward_clear(void);
/* Restarts both processors, the S32K first */
void be_remote_restart(void);
void be_remote_on_settings(const uint8_t* payload, uint16_t len);
void be_remote_on_ack(uint8_t type, uint8_t id, int16_t status);
#endif

#ifdef HW_MIV_S32K
/* S32K: sends the next part of the state through send(), one BE_LINK_SNAPSHOT payload per call, so that
 * one pass of the 1 ms core tick does not have to encode all of it. Returns true after the last part. */
bool be_snapshot_send_step(int (*send)(const void* payload, uint16_t len));
#endif

#ifdef CAN_VIA_LINK
/* ESP32 */
void be_remote_on_hello(uint8_t flags, uint32_t layout_hash);
void be_remote_on_snapshot(const uint8_t* payload, uint16_t len);
bool be_remote_active(void);
/* Replaces the local core logic in core_loop while remote */
void be_remote_tick(uint32_t now_ms);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BE_SNAPSHOT_H */
