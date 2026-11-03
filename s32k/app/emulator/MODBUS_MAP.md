# Modbus RTU register map, version 1 (S32K emulator, MIV variant A+)

Serial: RS485 on LPUART7 (FRDM-A-S32K312: PTB14 RX, PTB15 TX, PTB17 DE). Defaults: unit id 1,
19200 baud, 8E1. Unit id, baud and parity are settings (registers 5030–5032) and take effect
after a reboot.

All addresses are holding registers (FC03 read, FC06/FC16 write). The same addresses can also be
read as input registers (FC04). 32-bit values use two registers, high word first. Unused
addresses inside a block read as 0; addresses outside the blocks return exception 02.

## 0–99: system (read only)

| Reg | Meaning |
|---|---|
| 0 | map version (1) |
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
| 5040 | battery type (index of the upstream Battery-Emulator enum; stored, not used yet) | unlocked |
| 5041 | second battery type (same) | unlocked |
| 5042 | inverter type (same) | unlocked |

Every write while unlocked extends the unlock window. Writes while locked return exception 02.
An invalid configuration is refused by command 1 with result -22 (EINVAL) and nothing is saved.

## Planned blocks (next map versions)

- 200–499: emulator state (contactors, SOC/power sent to the inverter, limits)
- 500–999: events (counter + last N)
- 1000–1999, 2000–2999: battery 1 / battery 2 (nameplate, live values, limits)
- 3000–3499, 3500–3999: cell voltages of battery 1 / 2 (mV, one register per cell)

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
