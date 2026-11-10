# S32K312 firmware update over the web page (plan, version 3, 10.11.2026)

Goal: update the S32K application from the `/miv/update` page of the ESP32, without SWD, and get the old
image back when the new one does not come up. SWD stays the way to program the bootloader once.

**Decision of Dimitar (10.11.2026):** the S32K keeps a working image of its own (two slots, image limit
about 900 KB, way back without any ESP32). An ESP32 with 8 MB keeps a copy as well. The copy is an
addition, not a condition: everything works with the 4 MB ESP32 too, just without the copy.

Nothing is built yet. Facts were checked in the workspace (`zw/`, Zephyr v4.5.0, MCUboot in
`zw/bootloader/mcuboot`), in the current build (`zw/b/emu_mb`) and in its map file. Earlier versions
(two slots only; one slot + loader + store on the ESP32) are in the git history of this file.

## 1. Bootloader: MCUboot, swap with test and confirm

- MCUboot, mbedTLS and `west` are in the workspace; sysbuild builds MCUboot and the application
  together. All configuration goes into our board and application directories (`sysbuild.conf`,
  `sysbuild/mcuboot.conf`, `sysbuild/mcuboot.overlay`); `zw/zephyr` and `zw/bootloader` stay untouched.
- The C40 flash has a Zephyr driver (`nxp,c40-flash`, 8 KB sectors, 8 byte program unit). In the current
  image `FLASH_Program`, `FLASH_Erase` and their wait loops run from RAM (0x2040xxxx) with interrupts
  locked, so code can write the flash block it runs from: what a swap needs.
- **Mode: swap using move.** The new image goes to slot 1 as a test image; MCUboot swaps it in at the
  next start; the image confirms itself (`boot_write_img_confirmed()`, criteria in section 6). A start
  that does not confirm (reset, watchdog, power loss) makes MCUboot swap the old image back. MCUboot
  resumes an interrupted swap after a power loss. **The way back needs no ESP32.**
- Image check: SHA-256 in the image trailer now (`BOOT_SIGNATURE_TYPE_NONE`), ECDSA P-256 later
  (decision 1).
- No in-tree MCX E31x board uses MCUboot yet: this is a first. Nothing in MCUboot is specific to the SoC
  except the flash driver and the jump to the application.
- Points to handle: the S32K3 boot header (IVT) moves from the application to MCUboot at 0x400000 (the
  application builds with `BOARD_NXP_MCXE31X_BOOT_HEADER=n`); the MPU map of `be_mpu.c` (ERR011573) goes
  into MCUboot as a small Zephyr module added through sysbuild, not as a patch; MCUboot's log is off in
  the `emu_mb` variant (its console is the Modbus port there); the application keeps functional resets.

## 2. S32K flash layout (2000 KB usable)

Block 0 = 0x400000–0x4FFFFF, block 1 = 0x500000–0x5F3FFF; the last 48 KB belong to the HSE firmware.

| Partition | Offset | Size | Block |
|---|---|---|---|
| boot_partition (MCUboot + boot header) | 0x000000 | 64 KB | 0 |
| slot0 (running image) | 0x010000 | 920 KB (0xE6000) | 0 |
| spare | 0x0F6000 | 40 KB | 0 |
| slot1 (new image / old image after a swap) | 0x100000 | 912 KB (0xE4000) | 1 |
| storage (NVS) | 0x1E4000 | 64 KB, **same place as now**: settings survive | 1 |

- Swap using move needs the primary slot one sector bigger: 920 = 912 + 8 KB. 115 and 114 sectors,
  within MCUboot's default of 128.
- MCUboot itself about 25–35 KB (SHA-256 only), about 45 KB with ECDSA: 64 KB leave room.

## 3. Flash budget

| | KB |
|---|---|
| Slot 1, the smaller slot | 912 |
| MCUboot header in front of the image (vector table alignment) | −1 |
| Trailer: hash TLV and swap status (≈ 115 sectors × 3 × 8 B + magic), one sector reserved | −8 |
| **Largest image** | **≈ 903** |
| Image today (`emu_mb`, 712 488 B) | 696 |
| Update support in the application (link messages, slot writer, confirm logic) | +10–15 |
| Two batteries as two BMSs (`DUAL_BATTERY_PLAN.md`) | +10–20 |
| **Expected after both** | **≈ 716–731** |
| **Room left** | **≈ 170–185 KB (19–20 %)** |

Upstream grows mostly by battery drivers: 65 battery files make about 312 KB today, about 5 KB per
battery on average, factory code included. At 5–10 new batteries a year that is 25–50 KB a year: the
room lasts for years, and the savings below are ready when it does not.

**Where the image goes today** (from `zw/b/emu_mb/zephyr/zephyr.map`, without a new build; the sum of
the map lines is 12 % above the image, so the numbers are scaled by 0.88):

| Group | KB | Share |
|---|---|---|
| Battery drivers (65 files) | ≈ 312 | 44 % |
| of which the factory `BATTERIES.cpp` (constructors of every battery inlined into it) | ≈ 106 | |
| of which HTML renderers of the battery pages (4 files) | ≈ 26 | |
| C++ library (`libstdc++`, mostly locale and string instances) | ≈ 116 | 16 % |
| Rest of our port and upstream core (datalayer, safety, communication, UDS) | ≈ 129 | 18 % |
| Zephyr | ≈ 56 | 8 % |
| Inverter drivers (26 files) | ≈ 51 | 7 % |
| C library, NXP HAL, gcc runtime | ≈ 45 | 6 % |

**Savings when needed** (rough, before a build proves them):

| Way | Saves | Cost and risk |
|---|---|---|
| Leave out the battery HTML renderers on the S32K (the pages live on the ESP32) | ≈ 20–26 KB | Our CMake list plus empty stand-ins; no upstream change |
| Find what pulls the locale parts of `libstdc++` (a `stringstream` or `std::to_string` somewhere) and replace it in our port | up to 50–100 KB, to measure | Small if the user is in our code; a patch if it is in upstream code |
| Build only chosen batteries and inverters: a factory generated at build time from upstream's `BATTERIES.cpp` / `INVERTERS.cpp` | a dozen common batteries and inverters: ≈ 150 KB; Kona + Pylon only: ≈ 300 KB | The generator follows upstream's file; batteries not built cannot be chosen on site |
| Turn asserts off | 5–15 KB | Not recommended: they feed the fault path of the watchdog |

## 4. The update path (works with every ESP32)

1. The `/miv/update` view gets a second field, "S32K firmware" (`zephyr.signed.bin`). The browser sends
   it in 4 KB pieces through `POST /miv/api/s32k/ota/begin | chunk?offset=N | finish`, with
   `GET /miv/api/s32k/ota/state` for the progress bar.
2. The ESP32 forwards each piece over the link as `BE_LINK_OTA` (type 9, already reserved): BEGIN (first
   32 bytes of the image, size, SHA-256, version text), DATA (offset + up to 1000 bytes; the frame has a
   CRC-16), FINISH, ABORT. The S32K acknowledges each with `BE_LINK_ACK` (status + offset); the ESP32
   repeats a frame not acknowledged within 200 ms.
3. The **S32K application** (not a bootloader) writes slot 1 in its main thread: erase a sector when the
   write reaches it, write, read back. At FINISH it checks the image (section 6, rule 3), marks it as a
   test image (`boot_request_upgrade(BOOT_UPGRADE_TEST)`) and restarts after 2 s.
4. MCUboot swaps and starts the new image; the image confirms itself or MCUboot swaps back.

Expected time for 712 KB: transfer ≈ 6–8 s, erases ≈ 89 × 50 ms ≈ 4.5 s, HTTP ≈ 3 s → **about 15 s**,
then the swap at the start (estimate 5–10 s, measured in step 1). The S32K runs its old image (core
stopped) during the transfer and always has an application.

## 5. The copy on the 8 MB ESP32 (addition, last step)

Simplest useful role, no receiver of its own on the S32K:

- **The ESP32 keeps the S32K images that went through it:** the one now running on the S32K
  ("current") and the one before ("previous"). The update path of section 4 writes the pieces into a
  store slot on the ESP32 as it forwards them (or stores first and forwards after; same code).
- **The page can send a stored image again without a file:** "Install previous S32K firmware" and
  "Install current again" buttons. They go through the very same path (section 4) with the ESP32 as the
  source instead of the browser.
- **Nothing automatic:** the way back after a failed start is MCUboot's on the S32K. The ESP32 only
  records which image the S32K confirmed (from its version in HELLO/status) to label current and
  previous. If the S32K ever lost both of its slots (it should not), SWD or the "install" button brings
  an image back.
- Store slot format: a 4 KB header of our own (magic `S32KIMG1`, header version, image size, SHA-256 of
  the image, version text from the MCUboot header, state empty / written / verified / current /
  previous, a counter that orders the two, CRC-32 of the header), image from 4 KB on. A new image always
  goes into the slot that is not current.

**8 MB ESP32 partition table** (the ESP32 board Dimitar attaches; also the ESP32-S3-WROOM-1-N8R2 of the
MIV board):

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xE000 | 8 KB |
| app0 | app/ota_0 | 0x10000 | 3072 KB (0x300000) |
| app1 | app/ota_1 | 0x310000 | 3072 KB |
| coredump | data/coredump | 0x610000 | 64 KB |
| s32k_a | data/0x40 | 0x620000 | 912 KB (0xE4000) |
| s32k_b | data/0x40 | 0x704000 | 912 KB |
| (free) | | 0x7E8000 | 96 KB |

The store slots take an image of up to 908 KB (912 minus the 4 KB header), more than slot 1 of the S32K
takes. The ESP32 firmware gets 3 MB per slot (today 1.95 MB).

**4 MB ESP32 (`miv_4MB.csv`, unchanged):** everything in sections 1–4 works; only the stored copies and
the two "install" buttons are missing (the page asks for the file again).

## 6. Rules for the update (from the review, all kept)

1. **Watchdog from the bootloader on.** MCUboot starts SWT0 through its replaceable
   `mcuboot_watchdog_setup()` / `mcuboot_watchdog_feed()` (our module, SWT0 registers like `be_wdt.c`,
   not the Zephyr SWT driver), 10 s, fed between the sector steps of a swap. It stays on into the
   application; `be_wdt_start()` unlocks it and sets its own 500 ms. An image that hangs before
   `be_wdt_start()` is reset after 10 s, is not confirmed and is swapped back. Not hard locked; freeze in
   debug stays on.
2. **Confirm, the same for every configuration** (battery type 0 included): main thread alive, settings
   read from NVS without an error, link to the ESP32 up (HELLO received), 60 s since the start without a
   watchdog or fault reset. No link within 5 minutes → restart without a confirm → MCUboot swaps back.
   Before BEGIN the S32K writes "update to version X pending" to NVS, so the image that runs afterwards
   knows whether it is the new one or the old one back, and reports "update reverted".
3. **Checks of the file** before anything is written: MCUboot magic `0x96f3b83d` (an ESP32 file starts
   with `0xE9`), size within slot 1, no load address or encryption flags (all from the 32 byte header in
   BEGIN); at FINISH the vector table points into slot 0 and RAM, a board id TLV (imgtool
   `--custom-tlv`, tag 0xA0) equals the board's id, and the SHA-256 matches. The ESP32 checks the header
   first and refuses on the page.
4. **The link is quiet during an update.** The S32K receives with DMA into 2 × 512 bytes; erasing a
   sector locks the interrupts for 40–50 ms. One OTA frame in flight; the S32K erases only after a whole
   frame is in and before its ack; the ESP32 holds back settings, commands, live battery settings and
   bridge CAN frames (they wait in the queue of `be_snapshot.cpp`), only the 1 s HELLO goes out; the
   S32K refuses other messages and sends no snapshot or CAN.
5. **Web access.** `/miv/api/s32k/ota/*` sit behind the same global middleware as the upstream pages and
   `/ota` (web password, or local network only). The chunk body arrives before the middleware decides,
   so the chunk handler calls `webserver_request_authorized()` itself, as `/ota/upload` does.

## 7. Safety

- The S32K refuses BEGIN unless: the contactor outputs are off (Modbus 226 bits 0–3), `contactors_engaged`
  is 0, the emulator is paused (limits 0) and the current is below 1 A. The page says which failed.
- On BEGIN the core stops: no CAN to battery and inverter (a Kona opens its own contactors), outputs off
  until the restart.
- Nothing writes NVS during the update; NVS is outside both slots.
- Link silence over 10 s, ABORT or a failed check: slot 1 stays unmarked, the S32K restarts into its old
  image with the core as configured.

## 8. Programming and tests

- Once per board through SWD: MCUboot (`build/mcuboot/zephyr/zephyr.elf`, with the boot header) and the
  application (`build/emulator/zephyr/zephyr.signed.hex`, written to slot 0 as a confirmed image, so no
  swap follows). `flash_pe.sh` takes hex files as it takes an ELF; it gets the new paths and an
  `--app-only` mode.
- Development keeps SWD; sysbuild build directories (`zw/b/emu_mb/emulator/...`) and
  `run_s32k_tests.sh` point at the signed hex of each variant.
- To check in step 1: whether a PEmicro load still wipes the NVS (seen with changed images before). If
  so, the script loads by address range.

## 9. Steps and estimate

| Step | What | Days |
|---|---|---|
| 1 | Partitions in the board; sysbuild with MCUboot (swap using move, SHA-256); watchdog + MPU module; application without boot header. On the board with SWD: start through MCUboot; an image put into slot 1 and marked as test swaps in; without a confirm it swaps back; settings survive; swap time | 2–3 (+1–2 if MCUboot needs work on this part) |
| 2 | `BE_LINK_OTA` messages with acks, slot writer on the S32K, checks and safety conditions, confirm logic, quiet link; a PC script that sends an image through the ESP32 | 2–3 |
| 3 | `/miv/api/s32k/ota/*` streaming, the "S32K firmware" field in the Update view, progress | 1 |
| 4 | On the boards: full cycle; wrong file, ESP32 file, other board id refused; image that hangs in its first lines → back; image with a broken link → back after 5 min; power off during transfer and during the swap; refused with contactors closed and without the web password | 1–2 |
| | **Variant without the copy (works with the 4 MB ESP32)** | **6–9** |
| 5 | **Separable:** 8 MB ESP32: environment for the board, partition table, the two store slots with their header, writing them during a transfer, the "install current / previous" buttons, tests | 2 |
| | **Total** | **about 8–11 working days** |

A commit (no push) after every step, with measurements. Step 5 can wait without blocking anything
before it.

## Not included

- Production signing keys (decision 1); updating MCUboot over the link (SWD only); encrypted images,
  the HSE, downgrade protection; updating the S32K over Modbus/RS485 without an ESP32; updating the
  ESP32 from the S32K; the savings of section 3 (only listed).

## Decisions for Dimitar

1. **Signature:** SHA-256 now (protects against broken transfers, not against someone who can reach the
   page); ECDSA P-256 with the key outside the public repository before the first delivery. GPL-3.0: if
   the product refuses images not signed by MIV, users must get a way to install their own builds.
2. **The 8 MB ESP32 board** for step 5: which module, and which pins carry the W5500 and the UART to the
   S32K? It needs its own build environment until the MIV board with the S3 exists.
