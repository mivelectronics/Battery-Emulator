#include "be_snapshot.h"
#include "../contactorcontrol/comm_contactorcontrol.h"

#if defined(HW_MIV_S32K) || defined(CAN_VIA_LINK)

#include <Arduino.h>
#include <string.h>
#ifdef CAN_VIA_LINK
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <nvs.h>
#include "../can/can_link.h"
#endif

#include "../../battery/BATTERIES.h"
#include "../../datalayer/datalayer.h"
#include "../../devboard/safety/safety.h"
#include "../../devboard/utils/events.h"
#include "../../devboard/utils/logging.h"
#include "../../inverter/INVERTERS.h"
#include "be_link.h"

/* Explicit fields, not a copy of the datalayer structs: the two compilers size plain enums differently
   (short enums on the Arm build), so the layouts of the structs holding them differ. The aggregate and
   settings structs hold only fixed-width integers and bools and travel as they are; their sizes are
   part of the layout hash. */

enum SnapSection : uint8_t {
  SNAP_SYSTEM = 1,
  SNAP_AGGREGATE = 2,
  SNAP_SETTINGS = 3,
  SNAP_BATTERY = 4,
  SNAP_CELLS = 5,
  SNAP_EVENTS = 6,
};

// be_snap_system.flags
static constexpr uint16_t F_DC_BUS_LIVE = 1u << 0;
static constexpr uint16_t F_INVERTER_ALLOWS = 1u << 1;
static constexpr uint16_t F_BATTERY_ALLOWS = 1u << 2;
static constexpr uint16_t F_BATTERY2_ALLOWED = 1u << 3;
static constexpr uint16_t F_BATTERY3_ALLOWED = 1u << 4;
static constexpr uint16_t F_CONTACTORS2 = 1u << 5;
static constexpr uint16_t F_CONTACTORS3 = 1u << 6;
static constexpr uint16_t F_EQUIPMENT_STOP = 1u << 7;
static constexpr uint16_t F_BATTERY_DETECTED = 1u << 8;
static constexpr uint16_t F_ALLOWED_TO_SEND_CAN = 1u << 9;
static constexpr uint16_t F_BATTERY2_DETECTED = 1u << 10;
static constexpr uint16_t F_BATTERY3_DETECTED = 1u << 11;
static constexpr uint16_t F_CONTACTOR_CONTROL = 1u << 12;
static constexpr uint16_t F_CONTACTOR2_CONTROL = 1u << 13;

// be_snap_system.supports: what the real driver can do, so the pages offer the same buttons
#define BE_SUPPORTS(X)                                        \
  X(0, supports_real_BMS_status)                              \
  X(1, supports_charged_energy)                               \
  X(2, supports_insulation_resistance)                        \
  X(3, mandatory_charge_taper)                                \
  X(4, supports_clear_isolation)                              \
  X(5, supports_reset_BMS)                                    \
  X(6, supports_reset_SOC)                                    \
  X(7, supports_reset_crash)                                  \
  X(8, supports_reset_NVROL)                                  \
  X(9, supports_reset_DTC)                                    \
  X(10, supports_read_DTC)                                    \
  X(11, supports_reset_BECM)                                  \
  X(12, supports_calibrate_SOC)                               \
  X(13, supports_contactor_close)                             \
  X(14, supports_contactor_reset)                             \
  X(15, supports_manual_balancing)                            \
  X(16, supports_toggle_SOC_method)                           \
  X(17, supports_energy_saving_mode_reset)                    \
  X(18, supports_factory_mode_method)                         \
  X(19, supports_balancing)                                   \
  X(20, is_balancing_active)                                  \
  X(21, supports_balancing_request)                           \
  X(22, supports_isolation_test)

struct be_snap_system {
  uint8_t section;
  uint8_t system_status;
  uint8_t contactors_engaged;
  uint8_t precharge_status;
  uint8_t bms_reset_status;
  uint8_t can_inverter_still_alive;
  uint8_t configured_batteries;
  uint8_t reserved0;
  uint16_t flags;
  uint16_t battery_type;
  uint16_t inverter_type;
  uint16_t reserved;
  uint32_t supports;
  uint32_t settings_revision;
  uint32_t core_task_max_us;
  char battery_protocol[64];
  char shunt_protocol[32];
  char inverter_brand[8];
  char inverter_name[32];
  char battery_interface[8];
  char inverter_interface[8];
} BE_LINK_PACKED;

struct be_snap_aggregate {
  uint8_t section;
  uint8_t reserved[3];
  uint8_t data[sizeof(DATALAYER_AGGREGATE_TYPE)];
} BE_LINK_PACKED;

struct be_snap_settings {
  uint8_t section;
  uint8_t reserved[3];
  uint8_t data[sizeof(DATALAYER_BATTERY_SETTINGS_TYPE)];
} BE_LINK_PACKED;

struct be_snap_battery {
  uint8_t section;
  uint8_t pack;  // 1, 2 or 3
  uint8_t number_of_cells;
  uint8_t chemistry;
  uint8_t can_battery_still_alive;
  uint8_t real_bms_status;
  uint8_t balancing_status;
  uint8_t flags;  // bit 0 insulation available, 1 SOH available, 2 reports full, 3 reports empty
  // info
  uint32_t total_capacity_Wh;
  uint32_t reported_total_capacity_Wh;
  uint16_t max_design_voltage_dV;
  uint16_t min_design_voltage_dV;
  uint16_t max_cell_voltage_mV;
  uint16_t min_cell_voltage_mV;
  uint16_t max_cell_voltage_deviation_mV;
  uint16_t can_error_counter;
  // status
  uint32_t remaining_capacity_Wh;
  uint32_t reported_remaining_capacity_Wh;
  uint32_t max_discharge_power_W;
  uint32_t max_charge_power_W;
  uint32_t bms_max_discharge_power_W;
  uint32_t bms_max_charge_power_W;
  int32_t active_power_W;
  int32_t total_charged_battery_Wh;
  int32_t total_discharged_battery_Wh;
  uint16_t max_discharge_current_dA;
  uint16_t max_charge_current_dA;
  uint16_t soh_pptt;
  uint16_t voltage_dV;
  uint16_t cell_max_voltage_mV;
  uint16_t cell_min_voltage_mV;
  uint16_t real_soc;
  uint16_t reported_soc;
  uint16_t insulation_resistance_kOhm;
  int16_t temperature_max_dC;
  int16_t temperature_min_dC;
  int16_t current_dA;
  int16_t reported_current_dA;
} BE_LINK_PACKED;

struct be_snap_cells {
  uint8_t section;
  uint8_t pack;
  uint16_t count;
  uint8_t balancing[MAX_AMOUNT_CELLS / 8 + 1];
  uint16_t voltage_mV[MAX_AMOUNT_CELLS];  // only count entries are sent
} BE_LINK_PACKED;

struct be_snap_event {
  uint8_t id;
  uint8_t state;
  int16_t data;
} BE_LINK_PACKED;

static constexpr uint8_t MAX_SNAP_EVENTS = 64;

struct be_snap_events {
  uint8_t section;
  uint8_t count;
  be_snap_event events[MAX_SNAP_EVENTS];  // only count entries are sent
} BE_LINK_PACKED;

static_assert(sizeof(be_snap_system) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(sizeof(be_snap_aggregate) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(sizeof(be_snap_settings) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(sizeof(be_snap_battery) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(sizeof(be_snap_cells) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(sizeof(be_snap_events) <= BE_LINK_MAX_PAYLOAD, "does not fit a frame");
static_assert(EVENT_NOF_EVENTS <= 256, "be_snap_event.id is one byte");
static_assert(sizeof(DATALAYER_BATTERY_SETTINGS_TYPE) <= BE_SNAPSHOT_SETTINGS_MAX, "does not fit BE_SET_LIVE");

uint32_t be_snapshot_layout_hash(void) {
  // Anything that would make the two ends read each other's frames wrongly
  uint32_t h = 3;  // bump when the meaning of a field changes
  const uint32_t parts[] = {sizeof(be_snap_system),
                            sizeof(be_snap_aggregate),
                            sizeof(be_snap_settings),
                            sizeof(be_snap_battery),
                            sizeof(be_snap_cells),
                            sizeof(be_snap_events),
                            EVENT_NOF_EVENTS,
                            MAX_AMOUNT_CELLS,
                            (uint32_t)BatteryType::Highest,
                            (uint32_t)InverterProtocolType::Highest};
  for (uint32_t p : parts) {
    h = h * 31u + p;
  }
  return h;
}

static void copy_str(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

/* ---- S32K: sending ---------------------------------------------------------------------------- */
#ifdef HW_MIV_S32K

static const DATALAYER_BATTERY_TYPE& pack_of(uint8_t pack) {
  return pack == 3 ? datalayer.battery3 : (pack == 2 ? datalayer.battery2 : datalayer.battery);
}

static void send_pack(uint8_t pack, int (*send)(const void*, uint16_t)) {
  const DATALAYER_BATTERY_TYPE& b = pack_of(pack);
  const auto& i = b.info;
  const auto& s = b.status;
  static be_snap_battery m;

  m = {};
  m.section = SNAP_BATTERY;
  m.pack = pack;
  m.number_of_cells = i.number_of_cells;
  m.chemistry = (uint8_t)i.chemistry;
  m.can_battery_still_alive = s.CAN_battery_still_alive;
  m.real_bms_status = (uint8_t)s.real_bms_status;
  m.balancing_status = (uint8_t)s.balancing_status;
  m.flags = (s.insulation_resistance_available ? 1 : 0) | (s.soh_available ? 2 : 0) | (s.bms_reports_full ? 4 : 0) |
            (s.bms_reports_empty ? 8 : 0);
  m.total_capacity_Wh = i.total_capacity_Wh;
  m.reported_total_capacity_Wh = i.reported_total_capacity_Wh;
  m.max_design_voltage_dV = i.max_design_voltage_dV;
  m.min_design_voltage_dV = i.min_design_voltage_dV;
  m.max_cell_voltage_mV = i.max_cell_voltage_mV;
  m.min_cell_voltage_mV = i.min_cell_voltage_mV;
  m.max_cell_voltage_deviation_mV = i.max_cell_voltage_deviation_mV;
  m.can_error_counter = s.CAN_error_counter;
  m.remaining_capacity_Wh = s.remaining_capacity_Wh;
  m.reported_remaining_capacity_Wh = s.reported_remaining_capacity_Wh;
  m.max_discharge_power_W = s.max_discharge_power_W;
  m.max_charge_power_W = s.max_charge_power_W;
  m.bms_max_discharge_power_W = s.bms_max_discharge_power_W;
  m.bms_max_charge_power_W = s.bms_max_charge_power_W;
  m.active_power_W = s.active_power_W;
  m.total_charged_battery_Wh = s.total_charged_battery_Wh;
  m.total_discharged_battery_Wh = s.total_discharged_battery_Wh;
  m.max_discharge_current_dA = s.max_discharge_current_dA;
  m.max_charge_current_dA = s.max_charge_current_dA;
  m.soh_pptt = s.soh_pptt;
  m.voltage_dV = s.voltage_dV;
  m.cell_max_voltage_mV = s.cell_max_voltage_mV;
  m.cell_min_voltage_mV = s.cell_min_voltage_mV;
  m.real_soc = s.real_soc;
  m.reported_soc = s.reported_soc;
  m.insulation_resistance_kOhm = s.insulation_resistance_kOhm;
  m.temperature_max_dC = s.temperature_max_dC;
  m.temperature_min_dC = s.temperature_min_dC;
  m.current_dA = s.current_dA;
  m.reported_current_dA = s.reported_current_dA;
  send(&m, sizeof(m));
}

static void send_cells(uint8_t pack, int (*send)(const void*, uint16_t)) {
  const DATALAYER_BATTERY_TYPE& b = pack_of(pack);
  const auto& s = b.status;
  uint16_t count = b.info.number_of_cells;

  if (count == 0 || count > MAX_AMOUNT_CELLS) {
    return;
  }
  static be_snap_cells c;
  c.section = SNAP_CELLS;
  c.pack = pack;
  c.count = count;
  memset(c.balancing, 0, sizeof(c.balancing));
  for (uint16_t n = 0; n < count; n++) {
    c.voltage_mV[n] = s.cell_voltages_mV[n];
    if (s.cell_balancing_status[n]) {
      c.balancing[n / 8] |= (uint8_t)(1u << (n % 8));
    }
  }
  send(&c, offsetof(be_snap_cells, voltage_mV) + count * sizeof(uint16_t));
}

extern "C" uint32_t be_settings_revision(void);

static void send_system(int (*send)(const void*, uint16_t)) {
  const auto& st = datalayer.system.status;
  const auto& info = datalayer.system.info;

  static be_snap_system m;
  m = {};
  m.section = SNAP_SYSTEM;
  m.system_status = (uint8_t)st.system_status;
  m.contactors_engaged = st.contactors_engaged;
  m.precharge_status = (uint8_t)st.precharge_status;
  m.bms_reset_status = (uint8_t)st.bms_reset_status;
  m.can_inverter_still_alive = st.CAN_inverter_still_alive;
  m.configured_batteries = info.configured_batteries;
  if (battery) {
#define X(bit, method) m.supports |= battery->method() ? (1ul << bit) : 0;
    BE_SUPPORTS(X)
#undef X
    copy_str(m.battery_interface, sizeof(m.battery_interface), battery->interface_name());
  }
  m.flags = (st.dc_bus_live ? F_DC_BUS_LIVE : 0) | (st.inverter_allows_contactor_closing ? F_INVERTER_ALLOWS : 0) |
            (st.battery_allows_contactor_closing ? F_BATTERY_ALLOWS : 0) |
            (st.battery2_allowed_contactor_closing ? F_BATTERY2_ALLOWED : 0) |
            (st.battery3_allowed_contactor_closing ? F_BATTERY3_ALLOWED : 0) |
            (st.contactors_battery2_engaged ? F_CONTACTORS2 : 0) | (st.contactors_battery3_engaged ? F_CONTACTORS3 : 0) |
            (info.equipment_stop_active ? F_EQUIPMENT_STOP : 0) | (battery_detected ? F_BATTERY_DETECTED : 0) |
            (allowed_to_send_CAN ? F_ALLOWED_TO_SEND_CAN : 0) | (battery2_detected ? F_BATTERY2_DETECTED : 0) |
            (battery3_detected ? F_BATTERY3_DETECTED : 0) | (contactor_control_enabled ? F_CONTACTOR_CONTROL : 0) |
            (contactor_control_enabled_double_battery ? F_CONTACTOR2_CONTROL : 0);
  m.battery_type = (uint16_t)user_selected_battery_type;
  m.inverter_type = (uint16_t)user_selected_inverter_protocol;
  m.settings_revision = be_settings_revision();
  m.core_task_max_us = (uint32_t)st.core_task_max_us;
  copy_str(m.battery_protocol, sizeof(m.battery_protocol), info.battery_protocol);
  copy_str(m.shunt_protocol, sizeof(m.shunt_protocol), info.shunt_protocol);
  copy_str(m.inverter_brand, sizeof(m.inverter_brand), info.inverter_brand);
  if (inverter) {
    copy_str(m.inverter_name, sizeof(m.inverter_name), inverter->name());
    copy_str(m.inverter_interface, sizeof(m.inverter_interface), inverter->interface_name());
  }
  send(&m, sizeof(m));

}

bool be_snapshot_send_step(int (*send)(const void*, uint16_t)) {
  static uint8_t step = 0;

  switch (step) {
    case 0:
      send_system(send);
      break;
    case 1: {
      static be_snap_aggregate a;
      a.section = SNAP_AGGREGATE;
      memcpy(a.data, &datalayer.aggregate, sizeof(a.data));
      send(&a, sizeof(a));
    } break;
    case 2: {
      static be_snap_settings se;
      se.section = SNAP_SETTINGS;
      memcpy(se.data, &datalayer.battery_settings, sizeof(se.data));
      send(&se, sizeof(se));
    } break;
    case 3:
      send_pack(1, send);
      break;
    case 4:
      send_cells(1, send);
      break;
    case 5:
      if (battery2) {
        send_pack(2, send);
      }
      break;
    case 6:
      if (battery2) {
        send_cells(2, send);
      }
      break;
    case 7:
      if (battery3) {
        send_pack(3, send);
      }
      break;
    case 8:
      if (battery3) {
        send_cells(3, send);
      }
      break;
    default: {
      static be_snap_events e;
      e.section = SNAP_EVENTS;
      e.count = 0;
      for (int i = 0; i < EVENT_NOF_EVENTS && e.count < MAX_SNAP_EVENTS; i++) {
        const EVENTS_STRUCT_TYPE* ev = get_event_pointer((EVENTS_ENUM_TYPE)i);
        if (ev->state == EVENT_STATE_ACTIVE || ev->state == EVENT_STATE_ACTIVE_LATCHED) {
          e.events[e.count++] = {(uint8_t)i, (uint8_t)ev->state, ev->data};
        }
      }
      send(&e, offsetof(be_snap_events, events) + e.count * sizeof(be_snap_event));
      step = 0;
      return true;
    }
  }
  step++;
  return false;
}

#endif  // HW_MIV_S32K

/* ---- ESP32: receiving ------------------------------------------------------------------------- */
#ifdef CAN_VIA_LINK

// The drivers run on the S32K. These answer what the web pages and MQTT ask of a battery or an inverter.
static uint32_t remote_supports;
static char remote_battery_interface[8];
static char remote_inverter_interface[8];
static char remote_inverter_name[32];

class RemoteBattery : public Battery {
 public:
  void setup(void) override {}
  void update_values() override {}
  const char* interface_name() override { return remote_battery_interface; }

#define X(bit, method) \
  bool method() override { return (remote_supports >> bit) & 1; }
  BE_SUPPORTS(X)
#undef X

#define ACTION(method, code) \
  void method() override { be_remote_cmd(BE_CMD_BATTERY_ACTION, battery_index, code); }
  ACTION(request_isolation_test, BE_ACT_ISOLATION_TEST)
  ACTION(clear_isolation, BE_ACT_CLEAR_ISOLATION)
  ACTION(calibrate_SOC, BE_ACT_CALIBRATE_SOC)
  ACTION(reset_BMS, BE_ACT_RESET_BMS)
  ACTION(reset_SOC, BE_ACT_RESET_SOC)
  ACTION(reset_crash, BE_ACT_RESET_CRASH)
  ACTION(reset_contactor, BE_ACT_RESET_CONTACTOR)
  ACTION(reset_NVROL, BE_ACT_RESET_NVROL)
  ACTION(reset_DTC, BE_ACT_RESET_DTC)
  ACTION(read_DTC, BE_ACT_READ_DTC)
  ACTION(reset_BECM, BE_ACT_RESET_BECM)
  ACTION(request_open_contactors, BE_ACT_OPEN_CONTACTORS)
  ACTION(request_close_contactors, BE_ACT_CLOSE_CONTACTORS)
  ACTION(toggle_SOC_method, BE_ACT_TOGGLE_SOC_METHOD)
  ACTION(reset_energy_saving_mode, BE_ACT_RESET_ENERGY_SAVING)
  ACTION(set_factory_mode, BE_ACT_FACTORY_MODE)
  ACTION(initiate_balancing, BE_ACT_START_BALANCING)
  ACTION(end_balancing, BE_ACT_END_BALANCING)
#undef ACTION
};

class RemoteInverter : public InverterProtocol {
 public:
  const char* name() override { return remote_inverter_name; }
  const char* interface_name() override { return remote_inverter_interface; }
  InverterInterfaceType interface_type() override { return InverterInterfaceType::Can; }
  void update_values() override {}
};

static RemoteBattery remote_battery[3];
static RemoteInverter remote_inverter;

static bool active = false;
static bool hello_core_active = false;
static uint32_t last_hello_ms = 0;
static uint32_t last_snapshot_ms = 0;
static bool stale = false;
static bool layout_warned = false;
static uint32_t seen_settings_revision = 0;
static bool settings_revision_valid = false;
static uint32_t restart_at_ms = 0;

/* ---- Out to the S32K: one thing at a time, repeated until acknowledged ---- */
struct OutItem {
  uint8_t type;  // BE_LINK_CMD or BE_LINK_SETTINGS
  uint16_t len;
  uint8_t payload[4 + BE_SNAPSHOT_SETTINGS_MAX];
};
static constexpr int OUT_QUEUE_LEN = 96;
static constexpr uint32_t OUT_RETRY_MS = 150;
static constexpr int OUT_MAX_TRIES = 20;

static QueueHandle_t out_queue = nullptr;
static OutItem out_item;
static bool out_busy = false;
static uint16_t out_id = 0;
static uint32_t out_sent_ms = 0;
static int out_tries = 0;

static bool out_enqueue(uint8_t type, const void* payload, uint16_t len) {
  if (out_queue == nullptr || len > sizeof(OutItem::payload)) {
    return false;
  }
  OutItem item;
  item.type = type;
  item.len = len;
  memcpy(item.payload, payload, len);
  if (xQueueSend(out_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    logging.println("S32K link: outgoing queue full, a command or setting was lost");
    return false;
  }
  return true;
}

bool be_remote_cmd(uint16_t code, uint32_t arg0, uint32_t arg1) {
  struct be_link_cmd c = {};
  c.code = code;
  c.arg0 = arg0;
  c.arg1 = arg1;
  return out_enqueue(BE_LINK_CMD, &c, sizeof(c));
}

// These stay on this chip, passwords above all
static bool key_is_network(const char* key) {
  static const char* const prefixes[] = {"SSID",       "PASSWORD",  "WIFI",   "APPASSWORD",     "HOSTNAME",
                                         "MQTT",       "HADISC",    "HTTP",   "WEB",            "STATICIP",
                                         "LOCALIP",    "GATEWAY",   "SUBNET", "DNS",            "SYSLOG",
                                         "ESPNOW",     "USBENABLED", "CANLOG", "SDLOG",          "MEASURECPUTEMP",
                                         "CPUTEMPOFFSET", "PERFPROFILE"};
  for (const char* p : prefixes) {
    if (strncmp(key, p, strlen(p)) == 0) {
      return true;
    }
  }
  return false;
}

void be_remote_forward_setting(const char* key, uint8_t vtype, const void* value, uint16_t len) {
  if (!active || key_is_network(key)) {
    return;
  }
  struct be_set_kv m = {};
  if (strlen(key) >= sizeof(m.key) || len > sizeof(m.value)) {
    logging.printf("Setting %s is too long for the S32K link\n", key);
    return;
  }
  m.hdr.kind = BE_SET_KV;
  m.hdr.vtype = vtype;
  strncpy(m.key, key, sizeof(m.key) - 1);
  memcpy(m.value, value, len);
  out_enqueue(BE_LINK_SETTINGS, &m, offsetof(be_set_kv, value) + len);
}

void be_remote_forward_remove(const char* key) {
  if (!active || key_is_network(key) || strlen(key) >= BE_SET_KEY_LEN) {
    return;
  }
  struct be_set_kv m = {};
  m.hdr.kind = BE_SET_KV_REMOVE;
  strncpy(m.key, key, sizeof(m.key) - 1);
  out_enqueue(BE_LINK_SETTINGS, &m, offsetof(be_set_kv, value));
}

void be_remote_forward_clear(void) {
  if (active) {
    struct be_set_hdr h = {};
    h.kind = BE_SET_CLEAR_ALL;
    out_enqueue(BE_LINK_SETTINGS, &h, sizeof(h));
  }
}

void be_remote_restart(void) {
  // The S32K pauses, restarts after 5 s; this one follows when it is back up
  be_remote_cmd(BE_CMD_RESTART, 0, 0);
  const uint32_t now = millis() + 7000;
  restart_at_ms = now != 0 ? now : 1;
}

void be_remote_on_ack(uint8_t type, uint8_t id, int16_t status) {
  if (out_busy && type == out_item.type && id == (uint8_t)out_id) {
    out_busy = false;
    if (status != 0) {
      logging.printf("S32K refused a command or setting: %d\n", status);
    }
  }
}

static void out_pump(uint32_t now_ms) {
  if (!out_busy) {
    if (out_queue == nullptr || xQueueReceive(out_queue, &out_item, 0) != pdTRUE) {
      return;
    }
    if (++out_id == 0) {
      out_id = 1;
    }
    memcpy(out_item.payload + 2, &out_id, sizeof(out_id));  // id at offset 2 in both layouts
    out_busy = true;
    out_tries = 0;
    out_sent_ms = now_ms - OUT_RETRY_MS;
  }
  if (now_ms - out_sent_ms >= OUT_RETRY_MS) {
    if (out_tries >= OUT_MAX_TRIES) {
      logging.printf("S32K does not acknowledge a command or setting (type %u), dropped\n", out_item.type);
      out_busy = false;
      return;
    }
    out_tries++;
    out_sent_ms = now_ms;
    can_link_send_raw(out_item.type, out_item.payload, out_item.len);
  }
}

/* ---- Live settings: anything on this chip that writes battery_settings (web, MQTT) goes up as a whole */
static DATALAYER_BATTERY_SETTINGS_TYPE settings_shadow;
static bool settings_shadow_valid = false;

static bool send_local_settings_change() {
  if (!settings_shadow_valid || memcmp(&settings_shadow, &datalayer.battery_settings, sizeof(settings_shadow)) == 0) {
    return false;
  }
  memcpy(&settings_shadow, &datalayer.battery_settings, sizeof(settings_shadow));
  uint8_t buf[sizeof(be_set_hdr) + sizeof(settings_shadow)];
  be_set_hdr h = {};
  h.kind = BE_SET_LIVE;
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), &settings_shadow, sizeof(settings_shadow));
  out_enqueue(BE_LINK_SETTINGS, buf, sizeof(buf));
  return true;
}

/* ---- Settings coming back from the S32K: the page shows what it runs with ---- */
static constexpr int MAX_SYNC_KEYS = 128;
static char sync_seen[MAX_SYNC_KEYS][BE_SET_KEY_LEN];
static int sync_seen_count = 0;

static void apply_sync_kv(const be_set_kv& m, uint16_t value_len) {
  char key[BE_SET_KEY_LEN];
  memcpy(key, m.key, sizeof(key));
  key[sizeof(key) - 1] = 0;
  if (sync_seen_count < MAX_SYNC_KEYS) {
    memcpy(sync_seen[sync_seen_count++], key, sizeof(key));
  }
  // Directly, not through BatteryEmulatorSettingsStore: this must not be forwarded back
  Preferences p;
  if (!p.begin("batterySettings", false)) {
    return;
  }
  uint32_t v = 0;
  memcpy(&v, m.value, value_len < sizeof(v) ? value_len : sizeof(v));
  switch (m.hdr.vtype) {
    case 1:
      if (!p.isKey(key) || p.getInt(key, 0) != (int32_t)v) {
        p.putInt(key, (int32_t)v);
      }
      break;
    case 2:
      if (!p.isKey(key) || p.getUInt(key, 0) != v) {
        p.putUInt(key, v);
      }
      break;
    case 3:
      if (!p.isKey(key) || p.getBool(key, false) != (v != 0)) {
        p.putBool(key, v != 0);
      }
      break;
    case 4: {
      char text[BE_SET_VALUE_LEN + 1];
      memcpy(text, m.value, value_len);
      text[value_len] = 0;
      if (!p.isKey(key) || p.getString(key, "") != String(text)) {
        p.putString(key, text);
      }
    } break;
    default:
      break;
  }
  p.end();
}

// What the S32K does not hold falls back to the defaults it runs with, so the page must not keep it
static void finish_sync() {
  Preferences p;
  if (!p.begin("batterySettings", false)) {
    return;
  }
  char stale[32][BE_SET_KEY_LEN];
  int stale_count = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find("nvs", "batterySettings", NVS_TYPE_ANY, &it);
  while (err == ESP_OK && stale_count < 32) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    bool keep = key_is_network(info.key);
    for (int i = 0; !keep && i < sync_seen_count; i++) {
      keep = strncmp(sync_seen[i], info.key, BE_SET_KEY_LEN) == 0;
    }
    if (!keep) {
      strncpy(stale[stale_count], info.key, BE_SET_KEY_LEN - 1);
      stale[stale_count++][BE_SET_KEY_LEN - 1] = 0;
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  for (int i = 0; i < stale_count; i++) {
    p.remove(stale[i]);
  }
  p.end();
  sync_seen_count = 0;
}

void be_remote_on_settings(const uint8_t* payload, uint16_t len) {
  if (!active || len < sizeof(be_set_hdr)) {
    return;
  }
  if (payload[0] == BE_SET_SYNC_KV && len >= offsetof(be_set_kv, value) && len <= sizeof(be_set_kv)) {
    be_set_kv m = {};
    memcpy(&m, payload, len);
    apply_sync_kv(m, len - offsetof(be_set_kv, value));
  } else if (payload[0] == BE_SET_SYNC_END) {
    finish_sync();
  }
}

// Events this module raised on behalf of the S32K, so that its own stay untouched
static uint8_t mirrored[EVENT_NOF_EVENTS / 8 + 1];

static bool is_mirrored(int id) {
  return (mirrored[id / 8] >> (id % 8)) & 1;
}
static void set_mirrored(int id, bool on) {
  if (on) {
    mirrored[id / 8] |= (uint8_t)(1u << (id % 8));
  } else {
    mirrored[id / 8] &= (uint8_t)~(1u << (id % 8));
  }
}

void be_remote_on_hello(uint8_t flags, uint32_t layout_hash) {
  last_hello_ms = millis();
  hello_core_active = (flags & BE_HELLO_FLAG_CORE_ACTIVE) != 0;
  if (!hello_core_active || active) {
    return;
  }
  if (layout_hash != be_snapshot_layout_hash()) {
    if (!layout_warned) {
      layout_warned = true;
      logging.printf("S32K core runs, but its snapshot layout %08x differs from ours %08x: update the firmware\n",
                     (unsigned int)layout_hash, (unsigned int)be_snapshot_layout_hash());
    }
    return;
  }
  // From here on the S32K is the emulator: it also refuses CAN frames and commands from this side.
  // Any driver created from the local settings stays in memory, its transmit never runs again.
  battery = &remote_battery[0];
  battery2 = nullptr;
  battery3 = nullptr;
  inverter = &remote_inverter;
  last_snapshot_ms = millis();
  if (out_queue == nullptr) {
    out_queue = xQueueCreate(OUT_QUEUE_LEN, sizeof(OutItem));
  }
  active = true;
  logging.println("S32K core runs: web and MQTT now show its data, local core logic off");
}

bool be_remote_active(void) {
  return active;
}

static void apply_system(const be_snap_system& m) {
  auto& st = datalayer.system.status;
  auto& info = datalayer.system.info;

  remote_supports = m.supports;
  if (!settings_revision_valid || m.settings_revision != seen_settings_revision) {
    settings_revision_valid = true;
    seen_settings_revision = m.settings_revision;
    sync_seen_count = 0;
    be_remote_cmd(BE_CMD_SETTINGS_SYNC, 0, 0);
  }
  copy_str(remote_battery_interface, sizeof(remote_battery_interface), m.battery_interface);
  copy_str(remote_inverter_interface, sizeof(remote_inverter_interface), m.inverter_interface);
  copy_str(remote_inverter_name, sizeof(remote_inverter_name), m.inverter_name);

  st.contactors_engaged = m.contactors_engaged;
  st.precharge_status = (PrechargeState)m.precharge_status;
  st.bms_reset_status = (BMSResetState)m.bms_reset_status;
  st.CAN_inverter_still_alive = m.can_inverter_still_alive;
  st.dc_bus_live = (m.flags & F_DC_BUS_LIVE) != 0;
  st.inverter_allows_contactor_closing = (m.flags & F_INVERTER_ALLOWS) != 0;
  st.battery_allows_contactor_closing = (m.flags & F_BATTERY_ALLOWS) != 0;
  st.battery2_allowed_contactor_closing = (m.flags & F_BATTERY2_ALLOWED) != 0;
  st.battery3_allowed_contactor_closing = (m.flags & F_BATTERY3_ALLOWED) != 0;
  st.contactors_battery2_engaged = (m.flags & F_CONTACTORS2) != 0;
  st.contactors_battery3_engaged = (m.flags & F_CONTACTORS3) != 0;
  st.core_task_max_us = m.core_task_max_us;
  info.equipment_stop_active = (m.flags & F_EQUIPMENT_STOP) != 0;
  battery_detected = (m.flags & F_BATTERY_DETECTED) != 0;
  battery2_detected = (m.flags & F_BATTERY2_DETECTED) != 0;
  battery3_detected = (m.flags & F_BATTERY3_DETECTED) != 0;
  allowed_to_send_CAN = (m.flags & F_ALLOWED_TO_SEND_CAN) != 0;
  // What the status page shows: the S32K may have started with other settings than this side
  contactor_control_enabled = (m.flags & F_CONTACTOR_CONTROL) != 0;
  contactor_control_enabled_double_battery = (m.flags & F_CONTACTOR2_CONTROL) != 0;

  // In RAM only, so that the pages name the right types. Saving the settings page does not change the S32K.
  user_selected_battery_type = (BatteryType)m.battery_type;
  user_selected_inverter_protocol = (InverterProtocolType)m.inverter_type;

  copy_str(info.battery_protocol, sizeof(info.battery_protocol), m.battery_protocol);
  copy_str(info.shunt_protocol, sizeof(info.shunt_protocol), m.shunt_protocol);
  copy_str(info.inverter_brand, sizeof(info.inverter_brand), m.inverter_brand);

  info.configured_batteries = m.configured_batteries;
  battery2 = m.configured_batteries >= 2 ? &remote_battery[1] : nullptr;
  battery3 = m.configured_batteries >= 3 ? &remote_battery[2] : nullptr;
  remote_battery[1].battery_index = 2;
  remote_battery[2].battery_index = 3;
  user_selected_second_battery = m.configured_batteries >= 2;
  user_selected_triple_battery = m.configured_batteries >= 3;

  st.system_status = (system_status_enum)m.system_status;
}

static DATALAYER_BATTERY_TYPE* remote_pack(uint8_t pack) {
  switch (pack) {
    case 1:
      return &datalayer.battery;
    case 2:
      return &datalayer.battery2;
    case 3:
      return &datalayer.battery3;
    default:
      return nullptr;
  }
}

static void apply_battery(const be_snap_battery& m) {
  DATALAYER_BATTERY_TYPE* b = remote_pack(m.pack);

  if (b == nullptr) {
    return;
  }
  auto& i = b->info;
  auto& s = b->status;

  i.number_of_cells = m.number_of_cells;
  i.chemistry = (battery_chemistry_enum)m.chemistry;
  i.total_capacity_Wh = m.total_capacity_Wh;
  i.reported_total_capacity_Wh = m.reported_total_capacity_Wh;
  i.max_design_voltage_dV = m.max_design_voltage_dV;
  i.min_design_voltage_dV = m.min_design_voltage_dV;
  i.max_cell_voltage_mV = m.max_cell_voltage_mV;
  i.min_cell_voltage_mV = m.min_cell_voltage_mV;
  i.max_cell_voltage_deviation_mV = m.max_cell_voltage_deviation_mV;
  s.CAN_battery_still_alive = m.can_battery_still_alive;
  s.CAN_error_counter = m.can_error_counter;
  s.real_bms_status = (real_bms_status_enum)m.real_bms_status;
  s.balancing_status = (balancing_status_enum)m.balancing_status;
  s.insulation_resistance_available = (m.flags & 1) != 0;
  s.soh_available = (m.flags & 2) != 0;
  s.bms_reports_full = (m.flags & 4) != 0;
  s.bms_reports_empty = (m.flags & 8) != 0;
  s.remaining_capacity_Wh = m.remaining_capacity_Wh;
  s.reported_remaining_capacity_Wh = m.reported_remaining_capacity_Wh;
  s.max_discharge_power_W = m.max_discharge_power_W;
  s.max_charge_power_W = m.max_charge_power_W;
  s.bms_max_discharge_power_W = m.bms_max_discharge_power_W;
  s.bms_max_charge_power_W = m.bms_max_charge_power_W;
  s.active_power_W = m.active_power_W;
  s.total_charged_battery_Wh = m.total_charged_battery_Wh;
  s.total_discharged_battery_Wh = m.total_discharged_battery_Wh;
  s.max_discharge_current_dA = m.max_discharge_current_dA;
  s.max_charge_current_dA = m.max_charge_current_dA;
  s.soh_pptt = m.soh_pptt;
  s.voltage_dV = m.voltage_dV;
  s.cell_max_voltage_mV = m.cell_max_voltage_mV;
  s.cell_min_voltage_mV = m.cell_min_voltage_mV;
  s.real_soc = m.real_soc;
  s.reported_soc = m.reported_soc;
  s.insulation_resistance_kOhm = m.insulation_resistance_kOhm;
  s.temperature_max_dC = m.temperature_max_dC;
  s.temperature_min_dC = m.temperature_min_dC;
  s.current_dA = m.current_dA;
  s.reported_current_dA = m.reported_current_dA;
}

static void apply_events(const uint8_t* payload, uint16_t len) {
  if (len < offsetof(be_snap_events, events)) {
    return;
  }
  const uint8_t count = payload[1];

  if (len < offsetof(be_snap_events, events) + count * sizeof(be_snap_event)) {
    return;
  }
  bool in_list[EVENT_NOF_EVENTS] = {};

  for (uint8_t n = 0; n < count; n++) {
    be_snap_event e;
    memcpy(&e, payload + offsetof(be_snap_events, events) + n * sizeof(be_snap_event), sizeof(e));
    if (e.id >= EVENT_NOF_EVENTS) {
      continue;
    }
    in_list[e.id] = true;
    const EVENTS_ENUM_TYPE id = (EVENTS_ENUM_TYPE)e.id;
    const EVENTS_STATE_TYPE here = get_event_pointer(id)->state;

    if (here != EVENT_STATE_ACTIVE && here != EVENT_STATE_ACTIVE_LATCHED) {
      if (e.state == EVENT_STATE_ACTIVE_LATCHED) {
        set_event_latched(id, e.data);
      } else {
        set_event(id, e.data);
      }
      set_mirrored(e.id, true);
    }
  }
  for (int id = 0; id < EVENT_NOF_EVENTS; id++) {
    if (is_mirrored(id) && !in_list[id]) {
      clear_event((EVENTS_ENUM_TYPE)id);
      set_mirrored(id, false);
    }
  }
}

void be_remote_on_snapshot(const uint8_t* p, uint16_t len) {
  if (!active || len < 1) {
    return;
  }
  last_snapshot_ms = millis();

  switch (p[0]) {
    case SNAP_SYSTEM:
      if (len == sizeof(be_snap_system)) {
        be_snap_system m;
        memcpy(&m, p, sizeof(m));
        apply_system(m);
      }
      break;
    case SNAP_AGGREGATE:
      if (len == sizeof(be_snap_aggregate)) {
        memcpy(&datalayer.aggregate, p + offsetof(be_snap_aggregate, data), sizeof(datalayer.aggregate));
      }
      break;
    case SNAP_SETTINGS:
      if (len == sizeof(be_snap_settings) && !send_local_settings_change()) {
        memcpy(&datalayer.battery_settings, p + offsetof(be_snap_settings, data), sizeof(datalayer.battery_settings));
        memcpy(&settings_shadow, &datalayer.battery_settings, sizeof(settings_shadow));
        settings_shadow_valid = true;
      }
      break;
    case SNAP_BATTERY:
      if (len == sizeof(be_snap_battery)) {
        be_snap_battery m;
        memcpy(&m, p, sizeof(m));
        apply_battery(m);
      }
      break;
    case SNAP_CELLS: {
      const size_t head = offsetof(be_snap_cells, voltage_mV);
      if (len < head) {
        break;
      }
      const uint8_t pack = p[1];
      uint16_t count;
      memcpy(&count, p + 2, sizeof(count));
      DATALAYER_BATTERY_TYPE* b = remote_pack(pack);

      if (b == nullptr || count > MAX_AMOUNT_CELLS || len != head + count * sizeof(uint16_t)) {
        break;
      }
      memcpy(b->status.cell_voltages_mV, p + head, count * sizeof(uint16_t));
      const uint8_t* bal = p + offsetof(be_snap_cells, balancing);
      for (uint16_t n = 0; n < count; n++) {
        b->status.cell_balancing_status[n] = (bal[n / 8] >> (n % 8)) & 1;
      }
    } break;
    case SNAP_EVENTS:
      apply_events(p, len);
      break;
    default:
      break;
  }
}

void be_remote_tick(uint32_t now_ms) {
  static uint32_t next_1s = 0;

  send_local_settings_change();
  out_pump(now_ms);

  if (restart_at_ms != 0 && (int32_t)(now_ms - restart_at_ms) >= 0) {
    ESP.restart();
  }

  if ((int32_t)(now_ms - next_1s) < 0) {
    return;
  }
  next_1s = now_ms + 1000;

  // The S32K switched to bridge mode (battery type 0): the local core logic has to start from scratch
  if (!hello_core_active && (now_ms - last_hello_ms) < 3000 && restart_at_ms == 0) {
    logging.println("S32K core stopped: restarting to use the local settings");
    delay(100);
    ESP.restart();
  }

  // Figures of this chip, which is the one the page runs on
  datalayer.system.info.CPU_temperature =
      temperatureRead() + (float)datalayer.system.info.CPU_temperature_calibration_offset;
  datalayer.system.info.CPU_free_heap = ESP.getFreeHeap();

  // What is on the page is no longer true once the S32K goes quiet
  const bool quiet = (now_ms - last_snapshot_ms) > 5000;
  if (quiet && !stale) {
    logging.println("No data from the S32K for 5 s");
    datalayer.battery.status.CAN_battery_still_alive = 0;
    datalayer.battery2.status.CAN_battery_still_alive = 0;
    datalayer.battery3.status.CAN_battery_still_alive = 0;
    datalayer.system.status.CAN_inverter_still_alive = 0;
    datalayer.battery.status.real_bms_status = BMS_DISCONNECTED;
    set_event(EVENT_CAN_BATTERY_MISSING, 0);
    set_mirrored(EVENT_CAN_BATTERY_MISSING, true);
  }
  stale = quiet;
}

#endif  // CAN_VIA_LINK

#endif  // HW_MIV_S32K || CAN_VIA_LINK
