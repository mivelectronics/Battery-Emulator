/*
 * The Battery-Emulator core on the S32K: what setup() and core_loop() do in Software.cpp, without the
 * network, display and SD card parts. Software.cpp itself pulls in all of those, so it cannot be
 * compiled here. Its calculated values section is extracted at configure time (CMakeLists.txt) and
 * included below, so that logic stays upstream's, byte for byte.
 *
 * When upstream changes setup() or core_loop(), compare with be_core_setup() and be_core_tick().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <Arduino.h>
#include <list>

#include "src/battery/BATTERIES.h"
#include "src/communication/link/be_snapshot.h"
#include "src/communication/contactorcontrol/comm_contactorcontrol.h"
#include "src/charger/CHARGERS.h"
#include "src/communication/Transmitter.h"
#include "src/communication/can/comm_can.h"
#include "src/communication/contactorcontrol/comm_contactorcontrol.h"
#include "src/communication/nvm/comm_nvm.h"
#include "src/communication/precharge_control/precharge_control.h"
#include "src/communication/rs485/comm_rs485.h"
#include "src/datalayer/battery_aggregate.h"
#include "src/datalayer/datalayer.h"
#include "src/devboard/hal/hal.h"
#include "src/devboard/safety/parallel_safety.h"
#include "src/devboard/safety/safety.h"
#include "src/devboard/utils/events.h"
#include "src/devboard/utils/logging.h"
#include "src/devboard/utils/timer.h"
#include "src/devboard/utils/value_mapping.h"
#include "src/inverter/INVERTERS.h"

#include "be_core.h"
#include "be_link_cmd.h"

extern "C" {
#include "be_cfg.h"
int printk(const char* fmt, ...);
int link_send(uint8_t type, const void* payload, uint16_t len);
bool link_peer_alive(void);
}

static int send_snapshot(const void* payload, uint16_t len) {
  return link_send(2 /* BE_LINK_SNAPSHOT */, payload, len);
}

CAN_Interface be_core_can_interface(uint8_t iface);

const char* version_number = "s32k-emulator 0.3";

volatile uint32_t currentMillis = 0;
static uint32_t previousMillis10ms = 0;
static uint32_t previousMillisUpdateVal = 0;

Logging logging;

static std::list<Transmitter*> transmitters;
void register_transmitter(Transmitter* transmitter) {
  transmitters.push_back(transmitter);
}

// What the calculated values section asks of the ESP32. Not measured here yet.
static float temperatureRead() {
  return 0.0f;
}
extern "C" uint32_t port_free_heap(void);
// Upstream raises LOW_HEAP_MEMORY below 62 KB, a figure for the ESP32; the heap here is 16 KB in total
// and stops growing once the drivers exist. Only a really low value goes through. Modbus 221 has the truth.
struct PortEsp {
  uint32_t getFreeHeap() {
    const uint32_t free_bytes = port_free_heap();
    return free_bytes < 2000 ? free_bytes : 100000;
  }
};
static PortEsp port_esp;
#define ESP port_esp
#include "be_core_calc.inc"
#undef ESP

static void apply_be_cfg() {
  user_selected_battery_type = static_cast<BatteryType>(be_cfg.battery_type);
  user_selected_second_battery = be_cfg.battery2_type != 0;
  user_selected_inverter_protocol = static_cast<InverterProtocolType>(be_cfg.inverter_type);

  can_config.battery = be_core_can_interface(be_cfg.battery_iface);
  can_config.battery_double = be_core_can_interface(be_cfg.battery2_iface);
  can_config.inverter = be_core_can_interface(be_cfg.inverter_iface);

  datalayer.battery_settings.max_percentage = be_cfg.max_percentage;
  datalayer.battery_settings.min_percentage = be_cfg.min_percentage;
  datalayer.battery_settings.max_user_set_charge_dA = be_cfg.max_charge_dA;
  datalayer.battery_settings.max_user_set_discharge_dA = be_cfg.max_discharge_dA;
  datalayer.battery_settings.soc_scaling_active = be_cfg.soc_scaling != 0;
}

static volatile bool reload_requested = false;
void be_core_request_reload(void) {
  reload_requested = true;
}

void be_core_setup(void) {
  init_hal();
  init_events();
  init_stored_settings();
  apply_be_cfg();

  init_contactors();
  release_pins_across_reset();
  init_precharge_control();
  init_rs485();

  setup_charger();
  setup_inverter();
  setup_battery();

  if (battery && battery->mandatory_charge_taper()) {
    charge_taper_soc = true;
    if (charge_taper_band_pptt < 1500) {
      charge_taper_band_pptt = 1500;
    } else if (charge_taper_band_pptt > 5000) {
      charge_taper_band_pptt = 5000;
    }
  }

  setup_shunt();
  init_CAN();
}

static void battery_action(Battery* b, uint32_t action) {
  if (b == nullptr) {
    return;
  }
  switch (action) {
    case BE_ACT_ISOLATION_TEST:
      b->request_isolation_test();
      break;
    case BE_ACT_CLEAR_ISOLATION:
      b->clear_isolation();
      break;
    case BE_ACT_CALIBRATE_SOC:
      b->calibrate_SOC();
      break;
    case BE_ACT_RESET_BMS:
      b->reset_BMS();
      break;
    case BE_ACT_RESET_SOC:
      b->reset_SOC();
      break;
    case BE_ACT_RESET_CRASH:
      b->reset_crash();
      break;
    case BE_ACT_RESET_CONTACTOR:
      b->reset_contactor();
      break;
    case BE_ACT_RESET_NVROL:
      b->reset_NVROL();
      break;
    case BE_ACT_RESET_DTC:
      b->reset_DTC();
      break;
    case BE_ACT_READ_DTC:
      b->read_DTC();
      break;
    case BE_ACT_RESET_BECM:
      b->reset_BECM();
      break;
    case BE_ACT_OPEN_CONTACTORS:
      b->request_open_contactors();
      break;
    case BE_ACT_CLOSE_CONTACTORS:
      b->request_close_contactors();
      break;
    case BE_ACT_TOGGLE_SOC_METHOD:
      b->toggle_SOC_method();
      break;
    case BE_ACT_RESET_ENERGY_SAVING:
      b->reset_energy_saving_mode();
      break;
    case BE_ACT_FACTORY_MODE:
      b->set_factory_mode();
      break;
    case BE_ACT_START_BALANCING:
      b->initiate_balancing();
      break;
    case BE_ACT_END_BALANCING:
      b->end_balancing();
      break;
    default:
      break;
  }
}

static void handle_commands() {
  struct be_link_cmd c;

  while (be_link_cmd_pop_core(&c)) {
    switch (c.code) {
      case BE_CMD_BATTERY_ACTION:
        battery_action(c.arg0 == 3 ? battery3 : (c.arg0 == 2 ? battery2 : battery), c.arg1);
        break;
      case BE_CMD_PAUSE:
        setBatteryPause((c.arg0 & 1) != 0, (c.arg0 & 2) != 0, static_cast<EquipmentStop>(c.arg1));
        break;
      case BE_CMD_RESTART:
        // Stop charge/discharge first, like graceful_restart() on the ESP32
        setBatteryPause(true, false, EquipmentStop::UNCHANGED, false);
        be_cfg_system_reset_in(5000);
        break;
      case BE_CMD_BMS_RESET:
        start_bms_reset();
        break;
      case BE_CMD_CLEAR_EVENTS:
        reset_all_events();
        break;
      default:
        break;
    }
  }

  // Live settings from the web page or MQTT. The clocks of the two chips differ: what they time here
  // stays timed here.
  static uint8_t blob[sizeof(DATALAYER_BATTERY_SETTINGS_TYPE)];
  if (be_link_cmd_pop_live(blob, sizeof(blob)) == sizeof(blob)) {
    DATALAYER_BATTERY_SETTINGS_TYPE n;
    auto& cur = datalayer.battery_settings;

    memcpy(&n, blob, sizeof(n));
    n.balancing_start_time_ms = cur.balancing_start_time_ms;
    n.recovery_charge_start_time_ms = cur.recovery_charge_start_time_ms;
    n.remote_set_timestamp = (n.remote_set_timestamp != cur.remote_set_timestamp) ? millis() : cur.remote_set_timestamp;
    cur = n;
  }

  if (reload_requested) {
    reload_requested = false;
    init_stored_settings();
    apply_be_cfg();
  }
}

void be_core_tick(void) {
  static int loopPhase = 0;
  static bool snapshot_sending = false;

  handle_commands();

  receive_can();
  receive_rs485();
  if (shunt) {
    shunt->sample(millis());
  }

  currentMillis = millis();

  loopPhase = 1 - loopPhase;
  if (currentMillis - previousMillis10ms >= INTERVAL_10_MS && loopPhase == 0) {
    if ((currentMillis - previousMillis10ms >= INTERVAL_10_MS_DELAYED) &&
        (milliseconds(currentMillis) > esp32hal->BOOTUP_TIME())) {
      set_event(EVENT_TASK_OVERRUN, (currentMillis - previousMillis10ms));
    }
    previousMillis10ms = currentMillis;
    handle_contactors();
    if (precharge_control_enabled) {
      handle_precharge_control(currentMillis);
    }
    if (battery) {
      battery->handle_precharge();
    }
  }

  if (currentMillis - previousMillisUpdateVal >= INTERVAL_1_S && loopPhase == 1) {
    previousMillisUpdateVal = currentMillis;
    update_pause_state();

    if (battery) {
      battery->update_values();
    }
    if (battery2) {
      battery2->update_values();
      check_parallel_battery_safety(2);
    }
    if (battery3) {
      battery3->update_values();
      check_parallel_battery_safety(3);
    }
    update_calculated_values(currentMillis);
    update_machineryprotection(currentMillis);
    combine_pack_limits();
    filter_charge_taper_soc();
    filter_inverter_limits();
    update_aggregate_limits();

    if (inverter) {
      inverter->update_values();
    }
    update_restart_progress();

    snapshot_sending = link_peer_alive();
  }

  if (snapshot_sending) {
    snapshot_sending = !be_snapshot_send_step(send_snapshot);
  }

  for (auto& transmitter : transmitters) {
    transmitter->transmit(currentMillis);
  }
}

void be_core_print_status(void) {
  const DATALAYER_BATTERY_STATUS_TYPE& s = datalayer.battery.status;

  printk("core: status %d, SOC %u, %u dV, %d dA, cells %u, tick max %u us, heap free %u, stack free %u, rx drops %u, "
         "events level %d:",
         datalayer.system.status.system_status, s.real_soc, s.voltage_dV, s.current_dA,
         datalayer.battery.info.number_of_cells, be_core_tick_max_us, port_free_heap(), be_core_stack_free(),
         be_core_rx_drops, get_event_level());
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    const EVENTS_STRUCT_TYPE* e = get_event_pointer(static_cast<EVENTS_ENUM_TYPE>(i));

    if (e->state == EVENT_STATE_ACTIVE || e->state == EVENT_STATE_ACTIVE_LATCHED) {
      printk(" %s", get_event_enum_string(static_cast<EVENTS_ENUM_TYPE>(i)));
    }
  }
  printk("\n");
}

/* ---- Modbus registers (MODBUS_MAP.md) -------------------------------------------------------- */

static uint16_t hi16(uint32_t v) {
  return static_cast<uint16_t>(v >> 16);
}
static uint16_t lo16(uint32_t v) {
  return static_cast<uint16_t>(v);
}

static uint16_t reg_system(uint16_t r) {
  const auto& st = datalayer.system.status;
  const auto& ag = datalayer.aggregate;

  switch (r) {
    case 0:
      return 1;  // core running
    case 1:
      return static_cast<uint16_t>(user_selected_battery_type);
    case 2:
      return static_cast<uint16_t>(user_selected_inverter_protocol);
    case 3:
      return battery2 != nullptr;
    case 4:
      return st.system_status;
    case 5:
      return st.contactors_engaged;
    case 6:
      return st.inverter_allows_contactor_closing;
    case 7:
      return st.battery_allows_contactor_closing;
    case 8:
      return st.CAN_inverter_still_alive;
    case 10:
      return ag.reported_soc;
    case 11:
      return ag.voltage_dV;
    case 12:
      return static_cast<uint16_t>(ag.current_dA);
    case 13:
      return hi16(ag.max_charge_power_W);
    case 14:
      return lo16(ag.max_charge_power_W);
    case 15:
      return hi16(ag.max_discharge_power_W);
    case 16:
      return lo16(ag.max_discharge_power_W);
    case 17:
      return ag.max_charge_current_dA;
    case 18:
      return ag.max_discharge_current_dA;
    case 19:
      return static_cast<uint16_t>(ag.temperature_min_dC);
    case 20:
      return static_cast<uint16_t>(ag.temperature_max_dC);
    case 21:
      return static_cast<uint16_t>(port_free_heap() > 0xFFFF ? 0xFFFF : port_free_heap());
    case 22:
      return be_core_tick_max_us > 0xFFFF ? 0xFFFF : be_core_tick_max_us;
    case 23:
      return be_core_rx_drops > 0xFFFF ? 0xFFFF : be_core_rx_drops;
    case 24:
      return be_core_stack_free();
    case 25:
      return static_cast<uint16_t>(be_kv_count());
    default:
      return 0;
  }
}

static uint16_t reg_events(uint16_t r) {
  if (r == 0) {
    return EVENT_NOF_EVENTS;
  }
  if (r == 2) {
    return get_event_level();
  }
  uint16_t active = 0;
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    const EVENTS_STRUCT_TYPE* e = get_event_pointer(static_cast<EVENTS_ENUM_TYPE>(i));

    if (e->state == EVENT_STATE_ACTIVE || e->state == EVENT_STATE_ACTIVE_LATCHED) {
      if (r >= 10 && r - 10 == active) {
        return i;
      }
      active++;
    }
  }
  return r == 1 ? active : 0xFFFF;  // 10+: id of the n-th active event, 0xFFFF past the end
}

static uint16_t reg_battery(const DATALAYER_BATTERY_TYPE& b, uint16_t r) {
  const auto& i = b.info;
  const auto& s = b.status;

  switch (r) {
    case 0:
      return i.number_of_cells;
    case 1:
      return hi16(i.total_capacity_Wh);
    case 2:
      return lo16(i.total_capacity_Wh);
    case 3:
      return hi16(s.remaining_capacity_Wh);
    case 4:
      return lo16(s.remaining_capacity_Wh);
    case 5:
      return i.max_design_voltage_dV;
    case 6:
      return i.min_design_voltage_dV;
    case 7:
      return i.max_cell_voltage_mV;
    case 8:
      return i.min_cell_voltage_mV;
    case 10:
      return s.real_soc;
    case 11:
      return s.reported_soc;
    case 12:
      return s.soh_pptt;
    case 13:
      return s.voltage_dV;
    case 14:
      return static_cast<uint16_t>(s.current_dA);
    case 15:
      return static_cast<uint16_t>(s.reported_current_dA);
    case 16:
      return hi16(static_cast<uint32_t>(s.active_power_W));
    case 17:
      return lo16(static_cast<uint32_t>(s.active_power_W));
    case 18:
      return static_cast<uint16_t>(s.temperature_min_dC);
    case 19:
      return static_cast<uint16_t>(s.temperature_max_dC);
    case 20:
      return s.cell_max_voltage_mV;
    case 21:
      return s.cell_min_voltage_mV;
    case 22:
      return hi16(s.max_charge_power_W);
    case 23:
      return lo16(s.max_charge_power_W);
    case 24:
      return hi16(s.max_discharge_power_W);
    case 25:
      return lo16(s.max_discharge_power_W);
    case 26:
      return s.max_charge_current_dA;
    case 27:
      return s.max_discharge_current_dA;
    case 28:
      return s.CAN_battery_still_alive;
    case 29:
      return s.CAN_error_counter;
    case 30:
      return s.real_bms_status;
    case 31:
      return s.balancing_status;
    case 32:
      return s.insulation_resistance_kOhm;
    default:
      return 0;
  }
}

int be_core_reg_read(uint16_t addr, uint16_t* v) {
  if (addr >= 200 && addr < 300) {
    *v = reg_system(addr - 200);
  } else if (addr >= 500 && addr < 600) {
    *v = reg_events(addr - 500);
  } else if (addr >= 1000 && addr < 1100) {
    *v = reg_battery(datalayer.battery, addr - 1000);
  } else if (addr >= 2000 && addr < 2100) {
    *v = reg_battery(datalayer.battery2, addr - 2000);
  } else if (addr >= 3000 && addr < 3000 + MAX_AMOUNT_CELLS) {
    *v = datalayer.battery.status.cell_voltages_mV[addr - 3000];
  } else if (addr >= 3500 && addr < 3500 + MAX_AMOUNT_CELLS) {
    *v = datalayer.battery2.status.cell_voltages_mV[addr - 3500];
  } else {
    *v = 0;
  }
  return 0;
}
