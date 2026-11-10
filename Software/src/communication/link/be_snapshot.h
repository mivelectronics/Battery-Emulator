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

/* What only the S32K knows about itself: its CAN controllers, its watchdog and why it last started.
 * Travels as it is (fixed-width fields only) in the SNAP_S32K section. */
#define BE_S32K_NUM_CAN 3 /* CAN0, CAN1, CAN4 */
#define BE_S32K_F_WDT_RUNNING 0x01u
#define BE_S32K_F_LOCKOUT 0x02u /* contactors held open after 3 watchdog or fault restarts in a row */
#define BE_S32K_F_CORE 0x04u    /* the Battery-Emulator core runs */
#define BE_S32K_F_TEST_IMAGE 0x08u /* started by MCUboot as a test image, not confirmed yet */

struct be_s32k_can {
  uint8_t state; /* 0 error-active, 1 error-warning, 2 error-passive, 3 bus-off, 4 stopped */
  uint8_t tec;
  uint8_t rec;
  uint8_t reserved;
  uint16_t nominal_kbps; /* 0 = stopped */
  uint16_t data_kbps;    /* 0 = classic CAN */
  uint32_t rx;
  uint32_t tx_ok;
  uint32_t tx_err;
  uint32_t tx_drops; /* dropped from the transmit queue */
} __attribute__((packed));

struct be_s32k_status {
  uint8_t reset_cause;   /* enum be_reset_cause of the S32K, as Modbus register 10 */
  uint8_t resets_in_row; /* watchdog and fault restarts in a row */
  uint8_t flags;         /* BE_S32K_F_* */
  uint8_t reserved;
  uint16_t outputs;             /* pad levels, bits as Modbus register 226 */
  uint16_t outputs_after_reset; /* must be 0 */
  uint32_t uptime_s;
  uint32_t tick_max_us; /* longest core tick since start */
  uint32_t rx_drops;    /* received CAN frames dropped before the core saw them */
  char fw_version[24];
  struct be_s32k_can can[BE_S32K_NUM_CAN];
} __attribute__((packed));

#ifdef HW_MIV_S32K
/* S32K application: fills in the status above for the snapshot */
void be_s32k_status_fill(struct be_s32k_status* s);
#endif

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
/* ms since the last snapshot part arrived, UINT32_MAX if none since the start */
uint32_t be_remote_snapshot_age_ms(void);
/* The settings revision the S32K reported last, false if none yet */
bool be_remote_settings_revision(uint32_t* rev);
/* The last SNAP_S32K section, false if none arrived yet */
bool be_remote_s32k_status(struct be_s32k_status* out);
/* Odd while a snapshot part is being applied to the datalayer: a reader on another task copies what it
 * needs and tries again if the count moved (or is odd) */
uint32_t be_remote_apply_seq(void);
/* Replaces the local core logic in core_loop while remote */
void be_remote_tick(uint32_t now_ms);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BE_SNAPSHOT_H */
