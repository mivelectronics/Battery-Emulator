# S32K CAN processor (MIV hardware)

Battery-Emulator on MIV hardware is split over two chips: an ESP32 keeps the upstream network side
(web UI, MQTT, OTA, WiFi, Ethernet over W5500) and an NXP S32K3 under Zephyr does the CAN work.
The two talk over a UART with the protocol in `Software/src/communication/link/be_link.h`
(COBS frames with CRC-16, 2 Mbit/s).

## Layout

- `boards/nxp/frdm_a_s32k312/` — Zephyr board for the NXP FRDM-A-S32K312 (formerly S32K312MINI-EVB).
  The S32K312 is the same die as the MCX E317, so the board is built on Zephyr's MCXE31x SoC support
  (own `nxp_s32k312.dtsi`, because `nxp_mcxe317.dtsi` in Zephyr v4.5.0 refers to a missing label).
  Core 120 MHz from the 16 MHz crystal; console on LPUART6 (OpenSDA virtual COM).
- `boards/miv/miv_be_main/` — Zephyr board for the MIV main board be_main (schematic r13, `hw/be_main`): four CAN
  (two isolated inverter ports, the battery ports with TJA1448 standby), RS485, six VNQ7140AJ outputs with
  MultiSense, BMS supply switches, the socket for the ESP32 board. Same chip and start-up as the FRDM (it uses the
  FRDM's SoC devicetree, board.c and boot header). Pins and what is not checked: `boards/miv/miv_be_main/PINS.md`.
  The board does not exist yet: built only. Build with `-b miv_be_main`; OTA images with
  `BOARD=miv_be_main tools/build_ota.sh <variant>`.
- `app/bringup/` — clocks, LED, CAN0 FD loopback and normal mode, 10 ms timer jitter.
  Result on hardware: `app/bringup/bringup_2026-11-03.log`.
- `app/link_bridge/` — bridge mode: CAN0/CAN1/CAN4 <-> link UART (LPUART4). The ESP32 side is
  `Software/src/communication/can/can_link.cpp`, enabled with `-D CAN_VIA_LINK`.
- `tools/` — `setup_zephyr.sh` (Zephyr v4.5.0 + SDK 1.0.1), `zenv.sh` (source it),
  `flash_pe.sh` (flash over the on-board PEmicro OpenSDA), `serial_log.py`.

## Pins (FRDM-A-S32K312)

| Function | S32K312 | Notes |
|---|---|---|
| CAN0 (inverter) | PTA6 RX, PTA7 TX | on-board TJA1043, J21; EN PTC21, STB_N PTC20 |
| CAN1 (battery 1) | PTB23 RX, PTB22 TX | header, external transceiver |
| CAN4 (battery 2) | PTB2 RX, PTB3 TX | header, external transceiver |
| Link UART (LPUART4) | PTE10 RX, PTE11 TX | to ESP32 UART1: IO17 -> PTE10, PTE11 -> I39 (ESP32ETH_8R8I) or IO13 (ProDINo v15) |
| Console (LPUART6) | PTA15 RX, PTA16 TX | OpenSDA |

The FRDM board runs its I/O at 5 V by default. Before wiring it to an ESP32, move the VDD_HV_A
selector to 3.3 V: remove R91, fit 0 Ω on R98 (never both).

## Build and flash (Git Bash)

    source s32k/tools/zenv.sh
    cd "$ZW" && west build -b frdm_a_s32k312 -d b/bridge <repo>/s32k/app/link_bridge
    bash <repo>/s32k/tools/flash_pe.sh "$ZW/b/bridge/zephyr/zephyr.elf"

ESP32 side (PowerShell, not Git Bash: ESP-IDF tools refuse MSYS):

    $env:PLATFORMIO_CORE_DIR='C:\pio\core'; pio run -e miv_esp32eth_330

Two ESP32 boards, one firmware (hw_miv_esp32eth.h):

| | `miv_esp32eth_330` | `miv_prodino_v15` |
|---|---|---|
| Board | MIV ESP32ETH_8R8I_5Av5, 4 MB | KMP ProDINo ESP32 Ethernet v15, 8 MB |
| Link TX / RX | IO17 (J11-4) / I39 (J11-7) | IO17 (J11-4) / IO13 (J14-11) |
| GND for the link | J11-6 | J11-6 or J14-1/3 |
| I/O expander (held deselected) | MCP23S17, CS 32 | MCP23S08, CS 32, INT I36 |
| AP button | IO15 | none (RESET and FLASH only) |
| Partitions | `miv_4MB.csv`, 2 x 1.94 MB | `miv_8MB.csv`, 2 x 3 MB + two S32K image store slots |
| Power | | 8-30 V only; USB is an FT231X serial port |

W5500 (SCK 18, MISO 19, MOSI 23, CS 33, INT 5, RST 12), RS485 (RX 4, TX 16, DE 2) and the WS2812 on GPIO0
are the same on both.
