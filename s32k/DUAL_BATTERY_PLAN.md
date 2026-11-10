# Two batteries: in parallel as one BMS, or separately as two BMSs (plan, 10.11.2026)

Two modes Dimitar asked for:

- **Parallel:** two batteries → one BMS towards one inverter.
- **Separate:** two batteries → two independent BMSs, each towards its own inverter (or its own BMS
  port of one inverter).

Nothing is built for the separate mode yet. Facts were checked in the upstream code under `Software/src`
and in our S32K port (`s32k/app/emulator`).

## Hardware first (the MIV schematic waits for this)

The separate mode needs **a second CAN towards an inverter**. On the MIV board the second channel of
TJA1448A #2 is free but wired neither to the MCU nor to a connector.

FlexCAN pins of the S32K312 in the 172 HDQFP package (NXP pinctrl of the HAL,
`S32K312_172HDQFP-pinctrl.h`), against the pins the schematic of 09.11 already uses:

| FlexCAN | TX options | RX options | Free pair |
|---|---|---|---|
| CAN2 | PTD18 (taken: BMS_POWER), **PTE24** | **PTE25** | **PTE24 TX / PTE25 RX** |
| CAN3 | PTC0, PTC28, PTE9 | PTC1, PTC29 (taken: STOP), PTD15 (taken: AUX) | PTC0 / PTC1 |
| CAN5 | PTC10, PTC27, PTE12 | PTC11, PTC26, PTD17 (taken: ESP_BOOT) | PTC10 / PTC11, or PTC27 / PTC26 |

**Recommendation: FlexCAN2 on PTE24 (TX) / PTE25 (RX)**, to TXD2/RXD2 of TJA1448A #2, and its STB2 to a
free GPIO (or to GND, always normal, like the battery transceiver) instead of the 10k to 3V3 it has now.
Then a second inverter connector (RJ45 like the first, or terminals). FlexCAN2 sits next to CAN0/CAN1
in the peripheral map, takes classic CAN and CAN FD, and needs no other pin of the board. CAN5 on
PTC10/PTC11 is the second choice. The 129 pins from the third-party table still need the check against
NXP's IO table (SCHEMATIC_2026-11-09.md, open point 3): PTE24/PTE25 belong to that check.

**Contactor outputs:** the board has five (positive, negative, precharge, battery 2, spare) plus BMS
power. Separately with external contactors on both packs needs six (positive, negative, precharge for
each). With batteries that switch their own contactors over CAN (Kona and most car packs), no external
contactors are needed at all. Question 2 below.

## Parallel mode: what exists

- **Upstream** has it as "double battery" (`DBLBTR`, `battery2`, and up to three with `TRIBTR`): a second
  driver instance of the **same battery type** on its own CAN, the packs combined into
  `datalayer.aggregate` (`battery_aggregate.cpp`: SOC, limits, the parallel safety logic in
  `parallel_safety.cpp`), and the inverter driver reads the aggregate. One contactor output for battery
  2 that joins it after the precharge.
- **Our S32K port** builds it unchanged: Modbus 5041 (second battery), 5043/5044 (CAN channel of each
  pack, CAN1 and CAN4 by default), and battery 2 has its own Modbus block (2000+) and cells (3500+).
- **Tested so far only with the bench simulator in internal loopback** (FIRMWARE_PLAN, 04.11):
  two Kona packs, current −10.0 A in the combined view, 128 kWh on the web page, longest core tick
  404 µs. **Never on a wire**: CAN1 and CAN4 had no transceivers on the FRDM. The E51 has two channels
  and can play both packs once the MIV board (TJA1448A #1) or two external transceivers are there.
- Limits that stay as upstream has them: both packs the same type; a second pack on the inverter's bus
  is refused (same IDs).

So the parallel mode needs **no new code**, only a test on real wires (step P below).

## Separate mode: how bound upstream is

- **One inverter:** upstream has one global `InverterProtocol* inverter`, created by `setup_inverter()`
  from one type, on one interface (`can_config.inverter`).
- **One aggregate:** 24 of the 26 inverter drivers read `datalayer.aggregate`, 9 read
  `datalayer.battery` directly, and they read and write the inverter fields of
  `datalayer.system.status` (inverter alive, inverter allows contactor closing).
- **Battery drivers** keep their pack in `datalayer_battery` (per instance), but read the inverter's
  permission from the global `datalayer.system.status` (the Kona closes its contactors on CAN when the
  inverter allows it).
- **Good news:** the drivers keep their state in members (CAN frames, counters), not in file statics
  (checked over all 26 inverter files: the only file-scope statics are helper functions), so two
  instances of one protocol, even two Pylon, can live side by side.
- **But the inverter options are globals:** about 20 `user_selected_inverter_*` / `user_selected_pylon_*`
  variables in `INVERTERS.cpp` (Pylon send mode, cells, modules, voltage level, Sungrow/FoxESS type, the
  Deye workaround, the low pass filter, ...) that the drivers read. Either both sides use the same
  options, or side B's values are swapped in with its context (the plan does the latter; it is a list
  of plain variables).
- `datalayer` is 3104 bytes on the S32K (`datalayer_extended` 2024).

### Way without a heavy fork: two "sides" swapped in our core loop

The core loop on the S32K is ours (`port/be_core.cpp` repeats `setup()` and `core_loop()`), so the
separate mode lives there, without touching upstream files:

- **Side A** = battery 1 + inverter 1, exactly as today.
- **Side B** = battery 2 + inverter 2, run in the same 1 ms tick with its own context swapped into the
  globals around its calls: `datalayer.battery` holds pack 2's data, `datalayer.aggregate` side B's
  aggregate, the inverter fields of `datalayer.system.status` side B's, `can_config.inverter` side B's
  channel, the `user_selected_inverter_*` options side B's, `battery2`/`battery3` pointers empty. Then
  upstream's own functions compute side B
  (`update_calculated_values`, `combine_pack_limits`, `update_aggregate_limits`, the safety checks,
  `inverter2->update_values()`, its transmit and receive), and the context is swapped back. A swap is
  about 1.2 KB copied each way: a few µs at 120 MHz.
- The second inverter is created by calling upstream's `setup_inverter()` a second time with side B's
  type inside side B's context and keeping the pointer.
- Received CAN frames already go to every receiver on their channel (`port_can.cpp`); side B's
  receivers sit on side B's channels.
- **Events** stay one list; events of side B carry battery index 2 (upstream's `set_event(..., 2)`),
  "inverter missing" of side B gets data 2.
- **Contactors** per side: side A as today; side B uses the battery 2 output (and the spare one) with
  its own precharge sequence. This is the one place where new logic is needed (upstream joins battery 2
  to the main contactors).
- The ESP32 in local mode (core on the ESP32) does not get the separate mode: it exists only with the
  core on the S32K.

What may not survive the swap and has to be checked driver by driver: drivers that keep pointers to
`datalayer` fields taken at setup, and upstream code that keeps static state per inverter outside the
driver (watchdog of Modbus inverters, `inverter_modbus_watchdog_*`). CAN inverters first; RS485/Modbus
inverters for side B later or not at all (one RS485 port).

## Settings, web, snapshot, Modbus

- **Settings (`be_cfg` + NVS):** mode (single / parallel / separate); inverter 2 type and channel; for
  side B its own SOC window and current limits (side A keeps the existing ones). Checks: in separate
  mode the two batteries may be of different types; each side needs its own inverter channel; no
  channel shared between sides.
- **CAN:** a fourth channel in `can_io` (FlexCAN2), mapped in `port_can.cpp` onto a spare upstream
  `CAN_Interface` value; Modbus CAN block 160–179; SNAP_S32K carries four channels.
- **Snapshot:** a new section for side B (inverter 2 name, alive, what it was sent, side B aggregate);
  the layout hash changes, both boards are updated together.
- **Web:** `/miv/` shows two inverter cards and labels the battery cards "side A / side B". The upstream
  pages and MQTT keep showing side A only (their limit; MQTT for side B can come later).
- **Modbus:** inverter 2 / side B block (for example 250–299), settings 5058+.
- `MODBUS_MAP.md` gets the new registers.

## Flash and RAM

- Flash: the drivers are linked in already (712 KB); separate mode adds the swap, side B logic and
  settings: **about 10–20 KB**. Not the limit of variant 2 of the OTA plan (900 KB) either.
- RAM: the second inverter object (frames as members, about 0.5–2 KB depending on the protocol), side B
  context (aggregate + status fields + one pack copy, about 1.5 KB), a fourth CAN channel (queues, about
  2 KB). **About 4–6 KB.** SRAM is at 75 % (73.8 of 96 KB) and DTCM at 43 %: it fits, the CAN queues go
  to DTCM like the existing ones. Core tick with two sides: estimate below 1 ms (today 0.4 ms with two
  packs in parallel), to be measured.

## Steps and estimate

| Step | What | Days |
|---|---|---|
| P | Parallel mode on wires: E51 playing two Kona packs on CAN1 and CAN4 (MIV board or two transceivers on the FRDM), inverter on CAN0; combined values, battery 2 contactor timing, one pack lost | 1 |
| 1 | FlexCAN2 as a fourth channel (board, `can_io`, Modbus, SNAP_S32K), settings of the separate mode, side B context and the second inverter in `be_core.cpp`; simulator test with Kona + Kona and Pylon + Pylon in loopback | 3–4 |
| 2 | Contactors per side, safety and events per side, one side lost while the other keeps running | 2 |
| 3 | Snapshot section, `/miv/` cards, Modbus block and map | 1–2 |
| 4 | On wires with the E51 (two channels: one per side) and the real Kona + Deye when possible | 1–2 |
| | **Total** | **about 8–11 working days** (step P alone: 1 day) |

## Questions for Dimitar

1. Separate mode: towards **two inverters**, or **two BMS ports of one inverter** (Deye SUN-50K has
   BMS1/BMS2)? The code is the same, the test plan differs.
2. Batteries in the separate mode: with their **own contactors over CAN** (Kona and other car packs), or
   with **external contactors** (then three outputs per side: the board has five plus BMS power, one
   short)?
3. Different battery types per side allowed (for example Kona + BYD)? The plan allows it.
4. Second inverter CAN on the MIV board: **FlexCAN2 on PTE24/PTE25** to channel 2 of TJA1448A #2, STB2
   to GND or to a GPIO, and a second inverter connector: agreed?
