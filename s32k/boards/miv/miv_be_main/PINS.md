# MIV be_main (schematic r13): S32K312 pins in the firmware

Board `miv_be_main` (`s32k/boards/miv/miv_be_main`). Source of truth: the schematic `hw/be_main/*.kicad_sch`
(netlist exported 11.11.2026 with KiCad 10, the same as `hw/out/be_main.net` and `be_main_schematic_r13.pdf`).
Every pin below was compared with that netlist (net name, pin number of U1, the parts on the net) and with the NXP
IO signal table `hw/refs/s32k312/nxp_io/S32K312_IOMUX.xlsx` (the function exists on that pin, pin number in the
172 HDQFP column). The board has not been made: everything here is checked by building only.

## Pins

| S32K312 | Pin | Net | Function in the firmware | Start state | Sheet |
|---|---|---|---|---|---|
| PTA6 / PTA7 | 102 / 100 | CAN0_RX / CAN0_TX | FlexCAN0, inverter 1 (U2 ISO1044B, J8), no standby pin | CAN driver | 04 |
| PTE25 / PTE24 | 158 / 157 | CAN2_RX / CAN2_TX | FlexCAN2, inverter 2 (U8 ISO1044B, J7); described, the application does not use it | CAN driver | 04 |
| PTB23 / PTB22 | 69 / 67 | CAN1_RX / CAN1_TX | FlexCAN1, battery 1 (U3 TJA1448A ch. 1, J6) | CAN driver | 03 |
| PTB2 / PTB3 | 79 / 74 | CAN4_RX / CAN4_TX | FlexCAN4, battery 2 (U3 TJA1448A ch. 2, J5) | CAN driver | 03 |
| **PTC19** | **84** | CAN1_STB | `can_phy1` standby, active high (R56 10 k to 3V3); r13, was PTE21 | standby, normal from `can_start()` | 03 |
| PTE22 | 154 | CAN4_STB | `can_phy4` standby, active high (R57 10 k to 3V3) | standby, normal from `can_start()` | 03 |
| PTE11 / PTE10 | 9 / 10 | LINK_S2E / LINK_E2S | LPUART4, link to the ESP32 board (J13-5 / J13-6; R70 10 k pull-up on E2S), 2 Mbit/s, DMA | UART | 07 |
| PTD16 | 34 | ESP_EN | `esp_en`, EN of the ESP32-S3, active low, open drain by use (low or input, never high) | not touched (input) | 07 |
| PTD17 | 31 | ESP_BOOT | `esp_boot`, IO0 of the ESP32-S3, active low, as ESP_EN | not touched | 07 |
| PTD14 / PTD15 | 40 / 35 | AUX1 / AUX2 | `aux1` / `aux2`, J13-9 / J13-10, no function | not touched | 07 |
| PTB14 / PTB15 | 114 / 113 | RS485_RO / RS485_DI | LPUART7, Modbus RTU (U4 ST3485), RX with the internal pull-up | UART | 05 |
| PTB17 | 110 | RS485_DE | `modbus0` `de-gpios` (DE and /RE of U4) | Modbus driver | 05 |
| PTA16 / PTA15 | 143 / 145 | CONSOLE_TX / CONSOLE_RX | LPUART6, console on J15, 115200 | UART | 02 |
| PTA0 | 137 | OUT1_EN | `out1`, core pin 0 (positive contactor), U10 IN0, J9-1 | undriven, R30 100 k = off, until the core | 06 |
| PTA1 | 135 | OUT2_EN | `out2`, core pin 1 (negative contactor), U10 IN1, J9-2 | as OUT1 (R31) | 06 |
| PTA3 | 123 | OUT3_EN | `out3`, core pin 2 (precharge), U10 IN2, J9-3 | as OUT1 (R32) | 06 |
| PTA2 | 124 | OUT4_EN | `out4`, core pin 3 (battery 2 contactor), U10 IN3, J9-4 | as OUT1 (R33) | 06 |
| PTC6 | 141 | OUT5_EN | `out5`, U11 IN0, J9-5; not used by the core | output low at start | 06 |
| PTC7 | 140 | OUT6_EN | `out6`, U11 IN1, J9-6; not used by the core | output low at start | 06 |
| PTD18 | 105 | BMS_POWER | `bms_power`, core pin 4 (BMS power) = IG3, U12 IN1; JP10 1-2 also IG3_2 | undriven, R36 100 k = off, until the core | 06 |
| PTD20 | 111 | BMS_POWER2 | `bms_power2`, IG3_2 through JP10 2-3 (U12 IN3) | output low at start | 06 |
| PTD22 | 118 | BMS12_EN | `bms12_en`, +12V_BMS (U12 IN0); R94 not fitted, R46 100 k to GND | output low at start | 06 |
| PTD23 | 122 | BMS12_EN2 | `bms12_en2`, +12V_BMS2 (U12 IN2); R95 not fitted, R47 100 k to GND | output low at start | 06 |
| PTC20 / PTC21 / PTC23 | 85 / 86 / 87 | HS_SEL0 / 1 / 2 | `hs_sel0..2`, SEL0..2 of U10..U12 (15 k in series) | output low at start | 06 |
| PTE23 / PTE26 / PTA9 | 156 / 164 / 171 | HS_SEN_A / B / C | `hs_sen_a..c`, SEn of U10 / U11 / U12 | output low at start (MultiSense off) | 06 |
| PTD21 | 115 | HS_FRST | `hs_frst`, FaultRST of U10..U12 | output low at start (= auto restart, as undriven) | 06 |
| PTA8 | 172 | HS_ADC | `hs_adc` (alias `be-multisense`): ADC0 precise input P2, `adc_0` channel 0 | analog, nothing reads it yet | 06 |
| PTC29 | 99 | STOP | `stop_in`, core pin 5 (stop button), **active low**: R24 10 k to 3V3, contact J1-6 to GND | input, no internal pull | 02 |
| PTA29 / PTA30 / PTA31 | 32 / 33 / 39 | LED_CAN / LED_LINK / LED_ERR | D20 yellow / D21 green / D22 red, active low; aliases led2 / led1 / led0 | off | 02 |
| PTA4, PTC4, PTA10, PTC5, PTA5 | 170, 166, 161, 165, 167 | SWDIO, SWCLK, SWO, TDI, RESET_b | debug J14, not configured by the firmware | reset state | 02 |

The core pin numbers are those of `Software/src/devboard/hal/hw_miv_s32k.h`; the board overlay
`app/emulator/boards/miv_be_main.overlay` lists the lines in that order (`be-lines`). The FRDM keeps its list of
pins (`be-gpios`) in `app/emulator/boards/frdm_a_s32k312.overlay`, unchanged.

"Output low at start": `app/emulator/src/board_lines.c` drives every line marked `output-off-at-start` in the
board devicetree to its inactive level when the GPIO driver is up (POST_KERNEL), before `main()`. Between the reset
and that moment (and through MCUboot) the pins are inputs: the outputs are held off by the 100 k pull-downs (OUT,
BMS_POWER, BMS12_EN) or by the internal pull-downs of the VNQ7140AJ inputs (SEL, SEn, FaultRST).

Checked against the FRDM: PTC20 / PTC21 / PTC23 are EN / STB_N / ERR_N of the TJA1043 on the FRDM. On this board
they are HS_SEL0..2. Nothing of the TJA1043 is in this board's devicetree. CAN0 has no `phys`, and there is no
`enable-gpios`.

## Checked by building (11-12.11.2026)

- `miv_be_main`: the emulator builds plain, as an OTA image (MCUboot + application, `BOARD=miv_be_main
  tools/build_ota.sh prod`) and with the simulator (`-DBE_SIM=1`). There are no new warnings.
- The generated devicetree has 6 lines for the core (`be-lines`), 12 lines off at start, `phys` on CAN1 / CAN4
  with standby PTC19 / PTE22 active high, and DE on PTB17. The OTA image carries the board id `miv_be_main`
  (TLV 0xA0).
- `frdm_a_s32k312` before and after this change: the same `.config`, the same devicetree (apart from comments with
  the build path), and the same MCUboot binary. The application binary differs only in the order of two linker
  veneers. The object code of `be_gpio.c` is identical, and `board_lines.c` compiles to nothing.

## Not checked

- **Nothing on hardware.** The board does not exist; there were only builds.
- **The stop input is inverted against the FRDM.** On the FRDM the contact goes to 3.3 V with an internal
  pull-down. Here R24 pulls up and the contact goes to GND. The line is described active low, and the core reads it
  in its logical sense. A normally closed contact then behaves as on the FRDM: pressed or a broken wire = stop. If a
  normally open contact is meant, the flag has to change.
- **MultiSense on PTA8:** the ADC0_P2 mux is checked against the NXP table. The hardware channel number 2 of the
  Zephyr `nxp,sar-adc` driver (precise inputs = channels 0..7 of group 0) is taken from the driver code, not
  measured. Nothing reads it. The reading, the SEL / SEn sequence and the diagnostics are a separate task.
- **CAN2 (inverter 2)** is described and enabled, but the application has three CAN channels (CAN0, CAN1, CAN4)
  and does not use it.
- **BMS12_EN / BMS12_EN2** stay off: nothing in the firmware turns them on yet. With this firmware the BMS gets no
  +12V_BMS.
- **ESP_EN / ESP_BOOT / AUX** are only described: nothing drives them. The Zephyr SIUL2 GPIO driver has no open
  drain, so a reset of the ESP32 has to use output low and then input.
- **The crystal and clocks** are copied from the FRDM. The schematic has the same 16 MHz with CL 8 pF; the FXOSC
  settings (`delay`, `overdrive`) are the FRDM's and are not tuned for this crystal.
- **Flashing:** `board.cmake` names J-Link like the FRDM. On this board SWD is on J14 and needs an external probe.
  `tools/flash_pe.sh` is not tried on it.
- **The VNQ7140AJ inputs:** the levels (VIH / VIL through 15 k in series) and the standby when all inputs are low
  come from the schematic notes, not from tests.
