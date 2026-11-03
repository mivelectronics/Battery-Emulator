#pragma once

// CAN over the link UART to an S32K CAN processor (MIV hardware, bridge mode).
// Built only with -D CAN_VIA_LINK. The S32K runs s32k/app/link_bridge; see be_link.h for the protocol.
//
// Interface mapping on the S32K side:
//   iface 0 = CAN0 (on-board transceiver)     <-> CAN_NATIVE
//   iface 1 = CAN1 (CAN FD)                   <-> CANFD_ADDON_MCP2518 / CANFD_NATIVE
//   iface 2 = CAN4 (CAN FD)                   <-> CANFD_ADDON_MCP2518_2

#ifdef CAN_VIA_LINK

#include <stdint.h>
#include "../../devboard/utils/types.h"

// Opens the link UART (UART1, 2 Mbit/s) on the HAL's link pins. Safe to call more than once.
bool can_link_begin();

// Sets the bit rates of one S32K channel (data_kbps 0 = classic CAN).
bool can_link_configure(uint8_t iface, uint32_t nominal_kbps, uint32_t data_kbps);

// Queues one frame for transmission on an S32K channel. Returns false if the UART buffer is full.
bool can_link_send(uint8_t iface, const CAN_frame& frame);

// Processes received bytes and calls on_frame for every CAN frame that arrived.
void can_link_poll(void (*on_frame)(uint8_t iface, CAN_frame* frame));

// True while HELLO frames keep arriving from the S32K (within the last 3 s).
bool can_link_alive();

#endif  // CAN_VIA_LINK
