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
 * Read only for now: commands and settings from the ESP32 do not reach the S32K yet.
 *
 * Which mode: the HELLO of the S32K says whether its core runs (be_link_hello.reserved bit 0).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BE_HELLO_FLAG_CORE_ACTIVE 0x01u

/* Hash of the wire layout: HELLO carries it, and the ESP32 refuses a snapshot from a different build. */
uint32_t be_snapshot_layout_hash(void);

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
