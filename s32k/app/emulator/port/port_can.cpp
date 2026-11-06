/*
 * comm_can.h for the S32K: the upstream drivers talk to the FlexCAN channels through can_io.
 * The interfaces map the same way as on the ESP32 side of the link (comm_can.cpp, CAN_VIA_LINK).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <zephyr/sys/printk.h>

#include "src/communication/can/CanReceiver.h"
#include "src/communication/can/comm_can.h"

#include "be_core.h"

extern "C" {
int can_io_send_raw(uint8_t iface, uint32_t id, bool ext, bool fd, uint8_t len, const uint8_t* data);
int can_io_set_nominal(uint8_t iface, uint16_t kbps);
void can_io_pause(bool paused);
}

volatile CAN_Configuration can_config = {.battery = CANFD_ADDON_MCP2518,
                                         .inverter = CAN_NATIVE,
                                         .battery_double = CANFD_ADDON_MCP2518_2,
                                         .battery_triple = CAN_NATIVE,
                                         .charger = CAN_NATIVE,
                                         .shunt = CAN_NATIVE};

static const CAN_Interface iface_map[3] = {CAN_NATIVE, CANFD_ADDON_MCP2518, CANFD_ADDON_MCP2518_2};

// More than one on a channel: the battery and the inverter can share a bus
static constexpr int RECEIVERS_PER_CHANNEL = 3;
struct Channel {
  CanReceiver* receivers[RECEIVERS_PER_CHANNEL];
  int count;
  CAN_Speed speed;
};
static Channel channels[3];

static int to_iface(CAN_Interface interface) {
  for (int i = 0; i < 3; i++) {
    if (iface_map[i] == interface) {
      return i;
    }
  }
  return -1;
}

CAN_Interface be_core_can_interface(uint8_t iface) {
  return iface < 3 ? iface_map[iface] : CAN_NATIVE;
}

void register_can_receiver(CanReceiver* receiver, CAN_Interface interface, CAN_Speed speed) {
  int i = to_iface(interface);

  if (i < 0) {
    return;
  }
  Channel& c = channels[i];

  if (c.count == RECEIVERS_PER_CHANNEL) {
    printk("%s: no room for another CAN receiver\n", getCANInterfaceName(interface));
    return;
  }
  // One bus has one speed. The first one stays, and the other side raises its own missing event.
  if (c.count == 0) {
    c.speed = speed;
  } else if (c.speed != speed) {
    printk("%s: asked for %u kbps, runs at %u kbps\n", getCANInterfaceName(interface), static_cast<unsigned int>(speed),
           static_cast<unsigned int>(c.speed));
  }
  c.receivers[c.count++] = receiver;
}

bool init_CAN() {
  for (uint8_t i = 0; i < 3; i++) {
    if (channels[i].count > 0) {
      can_io_set_nominal(i, static_cast<uint16_t>(channels[i].speed));
    }
  }
  return true;
}

void receive_can() {
  struct be_can_rx rx;

  while (be_core_rx_pop(&rx)) {
    if (rx.iface >= 3) {
      continue;
    }
    CAN_frame frame = {};
    frame.FD = rx.fd != 0;
    frame.ext_ID = rx.ext != 0;
    frame.DLC = rx.len;
    frame.ID = rx.id;
    memcpy(frame.data.u8, rx.data, rx.len);
    for (int n = 0; n < channels[rx.iface].count; n++) {
      channels[rx.iface].receivers[n]->receive_can_frame(&frame);
    }
  }
}

void transmit_can_frame_to_interface(const CAN_frame* tx_frame, CAN_Interface interface) {
  int i = to_iface(interface);

  if (tx_frame != nullptr && i >= 0) {
    can_io_send_raw(i, tx_frame->ID, tx_frame->ext_ID, tx_frame->FD, tx_frame->DLC, tx_frame->data.u8);
  }
}

bool change_can_speed(CAN_Interface interface, CAN_Speed speed) {
  int i = to_iface(interface);

  return i >= 0 && can_io_set_nominal(i, static_cast<uint16_t>(speed)) == 0;
}

// update_pause_state() stops the CAN traffic towards the battery and the inverter with these
void stop_can() {
  can_io_pause(true);
}

void restart_can() {
  can_io_pause(false);
}

char const* getCANInterfaceName(CAN_Interface interface) {
  switch (to_iface(interface)) {
    case 0:
      return "CAN0";
    case 1:
      return "CAN1";
    case 2:
      return "CAN4";
    default:
      return "none";
  }
}

void dump_can_frame(CAN_frame& frame, CAN_Interface interface, frameDirection msgDir) {}

size_t format_can_frame(char* buffer, size_t len, const CAN_frame& frame, CAN_Interface interface,
                        frameDirection msgDir) {
  if (len > 0) {
    buffer[0] = 0;
  }
  return 0;
}
