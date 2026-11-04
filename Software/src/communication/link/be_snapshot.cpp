#include "be_snapshot.h"

#if defined(HW_MIV_S32K) || defined(CAN_VIA_LINK)

#include <Arduino.h>
#include <string.h>

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

// be_snap_system.supports: what the pages show. The action buttons (reset BMS, balancing, ...) stay
// off on the ESP32 until commands can reach the S32K.
static constexpr uint8_t S_REAL_BMS_STATUS = 1u << 0;
static constexpr uint8_t S_CHARGED_ENERGY = 1u << 1;
static constexpr uint8_t S_INSULATION = 1u << 2;
static constexpr uint8_t S_MANDATORY_TAPER = 1u << 3;

struct be_snap_system {
  uint8_t section;
  uint8_t system_status;
  uint8_t contactors_engaged;
  uint8_t precharge_status;
  uint8_t bms_reset_status;
  uint8_t can_inverter_still_alive;
  uint8_t configured_batteries;
  uint8_t supports;
  uint16_t flags;
  uint16_t battery_type;
  uint16_t inverter_type;
  uint16_t reserved;
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

uint32_t be_snapshot_layout_hash(void) {
  // Anything that would make the two ends read each other's frames wrongly
  uint32_t h = 2;  // bump when the meaning of a field changes
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
    m.supports = (battery->supports_real_BMS_status() ? S_REAL_BMS_STATUS : 0) |
                 (battery->supports_charged_energy() ? S_CHARGED_ENERGY : 0) |
                 (battery->supports_insulation_resistance() ? S_INSULATION : 0) |
                 (battery->mandatory_charge_taper() ? S_MANDATORY_TAPER : 0);
    copy_str(m.battery_interface, sizeof(m.battery_interface), battery->interface_name());
  }
  m.flags = (st.dc_bus_live ? F_DC_BUS_LIVE : 0) | (st.inverter_allows_contactor_closing ? F_INVERTER_ALLOWS : 0) |
            (st.battery_allows_contactor_closing ? F_BATTERY_ALLOWS : 0) |
            (st.battery2_allowed_contactor_closing ? F_BATTERY2_ALLOWED : 0) |
            (st.battery3_allowed_contactor_closing ? F_BATTERY3_ALLOWED : 0) |
            (st.contactors_battery2_engaged ? F_CONTACTORS2 : 0) | (st.contactors_battery3_engaged ? F_CONTACTORS3 : 0) |
            (info.equipment_stop_active ? F_EQUIPMENT_STOP : 0) | (battery_detected ? F_BATTERY_DETECTED : 0) |
            (allowed_to_send_CAN ? F_ALLOWED_TO_SEND_CAN : 0) | (battery2_detected ? F_BATTERY2_DETECTED : 0) |
            (battery3_detected ? F_BATTERY3_DETECTED : 0);
  m.battery_type = (uint16_t)user_selected_battery_type;
  m.inverter_type = (uint16_t)user_selected_inverter_protocol;
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
static uint8_t remote_supports;
static char remote_battery_interface[8];
static char remote_inverter_interface[8];
static char remote_inverter_name[32];

class RemoteBattery : public Battery {
 public:
  void setup(void) override {}
  void update_values() override {}
  const char* interface_name() override { return remote_battery_interface; }
  bool mandatory_charge_taper() override { return (remote_supports & S_MANDATORY_TAPER) != 0; }
  bool supports_real_BMS_status() override { return (remote_supports & S_REAL_BMS_STATUS) != 0; }
  bool supports_charged_energy() override { return (remote_supports & S_CHARGED_ENERGY) != 0; }
  bool supports_insulation_resistance() override { return (remote_supports & S_INSULATION) != 0; }
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
      if (len == sizeof(be_snap_settings)) {
        memcpy(&datalayer.battery_settings, p + offsetof(be_snap_settings, data), sizeof(datalayer.battery_settings));
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

  if ((int32_t)(now_ms - next_1s) < 0) {
    return;
  }
  next_1s = now_ms + 1000;

  // The S32K switched to bridge mode (battery type 0): the local core logic has to start from scratch
  if (!hello_core_active && (now_ms - last_hello_ms) < 3000) {
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
