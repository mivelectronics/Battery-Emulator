# Modbus RTU register map, version 2 (S32K emulator, MIV variant A+)

Serial: RS485 on LPUART7 (FRDM-A-S32K312: PTB14 RX, PTB15 TX, PTB17 DE). Defaults: unit id 1,
19200 baud, 8E1. Unit id, baud and parity are settings (registers 5030–5032) and take effect
after a reboot.

All addresses are holding registers (FC03 read, FC06/FC16 write). The same addresses can also be
read as input registers (FC04). 32-bit values use two registers, high word first. Unused
addresses inside a block read as 0; addresses outside the blocks return exception 02.

## 0–99: system (read only)

| Reg | Meaning |
|---|---|
| 0 | map version (2) |
| 1 | firmware version, major << 8 \| minor |
| 2–3 | uptime, seconds (u32) |
| 4–5 | settings revision (u32), increments on every save |
| 6 | network module (ESP32) present on the link: 1/0 |
| 7 | settings unlocked: 1/0 |
| 8 | result of the last command (0 ok, negative errno as int16) |
| 9 | unsaved edits in the staging copy: 1/0 |

## 100–159: CAN channels (read only), 20 registers per channel

Channel 0 = CAN0 (inverter, base 100), 1 = CAN1 (battery 1, base 120), 2 = CAN4 (battery 2, base 140).

| Offset | Meaning |
|---|---|
| +0 | state: 0 error-active, 1 error-warning, 2 error-passive, 3 bus-off, 4 stopped |
| +1 | transmit error counter |
| +2 | receive error counter |
| +3–4 | received frames (u32) |
| +5–6 | transmitted frames (u32) |
| +7–8 | transmit errors (u32) |
| +9 | running arbitration bit rate, kbit/s (0 = stopped) |
| +10 | running data bit rate, kbit/s (0 = classic CAN) |
| +11–12 | frames dropped from the transmit queue (u32) |

## 5000–5099: settings

Settings registers show and edit a staging copy. Nothing changes in operation until command 1.

| Reg | Meaning | Write |
|---|---|---|
| 5000 | unlock: write 0xA5C3 (42435) to unlock for 60 s; any other value locks. Reads 1 while unlocked | always |
| 5001 | command: 1 save to NVS and apply, 2 discard edits (reload from NVS), 3 load defaults (not saved until 1), 0xB007 reboot. Reads the last result | unlocked |
| 5010 + 4·i | CAN channel i: arbitration bit rate, kbit/s (100, 125, 200, 250, 500, 800, 1000) | unlocked |
| 5011 + 4·i | CAN channel i: data bit rate, kbit/s, 0 = classic CAN (≥ arbitration, ≤ 5000) | unlocked |
| 5012 + 4·i | CAN channel i: enabled 1/0 | unlocked |
| 5030 | Modbus unit id (1–247), after reboot | unlocked |
| 5031 | Modbus baud / 100 (96, 192, 384, 576, 1152), after reboot | unlocked |
| 5032 | Modbus parity: 0 none (2 stop bits), 1 odd, 2 even | unlocked |
| 5040 | battery type: index of the upstream `BatteryType` enum (17 = Kia/Hyundai 64, 40 = Kia 64 FD). 0 = core off, CAN bridged to the ESP32. After reboot | unlocked |
| 5041 | second battery in parallel, same type as the first: any value other than 0 turns it on. After reboot | unlocked |
| 5042 | inverter protocol: index of the upstream `InverterProtocolType` enum (10 = Pylon). After reboot | unlocked |
| 5043 | CAN channel of battery 1: 0 = CAN0, 1 = CAN1, 2 = CAN4 (default 1). After reboot | unlocked |
| 5044 | CAN channel of battery 2 (default 2). After reboot | unlocked |
| 5045 | CAN channel of the inverter (default 0). After reboot | unlocked |
| 5046 | SOC scaling towards the inverter: 1/0. After reboot | unlocked |
| 5050 | top of the SOC window, 0.01 % (default 8000). After reboot | unlocked |
| 5051 | bottom of the SOC window, 0.01 %, int16 (default 2000). After reboot | unlocked |
| 5052 | maximum charge current, 0.1 A (default 300). After reboot | unlocked |
| 5053 | maximum discharge current, 0.1 A (default 300). After reboot | unlocked |

Every write while unlocked extends the unlock window. Writes while locked return exception 02.
An invalid configuration is refused by command 1 with result -22 (EINVAL) and nothing is saved.

## 200–299: emulator state (read only, 0 while the core is off)

| Reg | Meaning |
|---|---|
| 200 | core running: 1/0 |
| 201 | battery type |
| 202 | inverter protocol |
| 203 | second battery present: 1/0 |
| 204 | system status: 0 standby, 1 inactive, 3 active, 4 fault, 5 updating |
| 205 | contactors engaged (upstream `contactors_engaged`) |
| 206 | inverter allows contactor closing: 1/0 |
| 207 | battery allows contactor closing: 1/0 |
| 208 | inverter alive counter, above 0 = heard recently |
| 210 | SOC sent to the inverter, 0.01 % |
| 211 | voltage, 0.1 V |
| 212 | current, 0.1 A, int16 |
| 213–214 | charge power limit, W (u32) |
| 215–216 | discharge power limit, W (u32) |
| 217 | charge current limit, 0.1 A |
| 218 | discharge current limit, 0.1 A |
| 219 | lowest temperature, 0.1 °C, int16 |
| 220 | highest temperature, 0.1 °C, int16 |
| 221 | free heap, bytes (capped at 65535) |
| 222 | longest core tick since start, µs (the tick runs every 1 ms) |
| 223 | received CAN frames dropped (queue full) |
| 224 | free stack of the core thread, bytes |
| 225 | number of upstream settings stored by name, those saved from the ESP32 web page |

210–220 are the combined view of all packs, what the inverter driver reads.

## 500–599: events (read only)

| Reg | Meaning |
|---|---|
| 500 | number of event types (ids run from 0 to this − 1) |
| 501 | number of active events |
| 502 | highest active level: 0 info, 1 debug, 2 warning, 3 error, as in upstream `get_event_level` |
| 510–599 | ids of the active events in id order, 0xFFFF past the end. Names: upstream `events.h` |

## 1000–1099, 2000–2099: battery 1, battery 2 (read only)

| Offset | Meaning |
|---|---|
| +0 | number of cells |
| +1–2 | capacity, Wh (u32) |
| +3–4 | remaining capacity, Wh (u32) |
| +5, +6 | design voltage maximum, minimum, 0.1 V |
| +7, +8 | cell voltage maximum, minimum allowed, mV |
| +10 | SOC from the battery, 0.01 % |
| +11 | SOC after scaling, 0.01 % |
| +12 | SOH, 0.01 % |
| +13 | voltage, 0.1 V |
| +14 | current, 0.1 A, int16, negative = discharging |
| +15 | current after scaling, 0.1 A, int16 |
| +16–17 | power, W (int32) |
| +18, +19 | temperature lowest, highest, 0.1 °C, int16 |
| +20, +21 | cell voltage highest, lowest, mV |
| +22–23 | charge power limit, W (u32) |
| +24–25 | discharge power limit, W (u32) |
| +26, +27 | charge, discharge current limit, 0.1 A |
| +28 | battery alive counter, above 0 = heard recently |
| +29 | CAN errors |
| +30 | BMS status (upstream `real_bms_status_enum`) |
| +31 | balancing status (upstream `balancing_status_enum`) |
| +32 | insulation resistance, kΩ |

## 3000–3191, 3500–3691: cell voltages of battery 1, battery 2 (read only, mV, one register per cell)

## 400–409: bench simulator (only in builds with BE_SIM)

| Reg | Meaning |
|---|---|
| 400 | simulator present: 1 |
| 401–408 | bytes of the last 0x4210 frame the inverter driver sent |
| 409 | count of those frames |
| 410 | UDS requests to the battery other than the regular polls |
| 411 | stored settings with a network name (must stay 0: they stay on the ESP32) |

## Home Assistant example

```yaml
modbus:
  - name: battery_emulator
    type: serial
    port: /dev/ttyUSB0
    baudrate: 19200
    bytesize: 8
    method: rtu
    parity: E
    stopbits: 1
    sensors:
      - name: "BE uptime"
        slave: 1
        address: 2
        data_type: uint32
        unit_of_measurement: s
      - name: "BE inverter CAN state"
        slave: 1
        address: 100
      - name: "BE battery 1 CAN rx frames"
        slave: 1
        address: 123
        data_type: uint32
```
