# S32K312 firmware update over the web page (plan, version 2, 10.11.2026)

Goal: update the S32K application from the `/miv/update` page of the ESP32, without SWD, and get the old
image back when the new one does not come up. SWD stays the way to program the bootloader once.

Version 2 follows Dimitar's change of 10.11: the S32K flash is not split in half; the copy of the old,
working image is kept by an ESP32 with 8 MB of flash. Version 1 (two slots on the S32K) stays here as
variant 2 for comparison. Nothing is built yet. Facts were checked in the workspace (`zw/`, Zephyr
v4.5.0, MCUboot in `zw/bootloader/mcuboot`) and in the current build (`zw/b/emu_mb`).

## Recommendation

**Variant 1: MCUboot with one application slot on the S32K, a small firmware loader on the S32K that
speaks `be_link`, and the image store (current + previous) on the ESP32.** It gives the application
about 1.7 MB instead of about 900 KB, keeps the NVS settings where they are, and reuses MCUboot for the
image check. Its price: without an ESP32 there is no update and no way back, and while an image is being
written the S32K has no application (contactor outputs off). On the 4 MB ESP32 board the same works
without the stored copy (see "ESP32 flash").

## Facts that hold for every variant

- MCUboot, mbedTLS and `west` are in the workspace; sysbuild builds bootloader + images together. All
  configuration goes into our board and application directories; `zw/zephyr` and `zw/bootloader` stay
  untouched.
- The C40 flash has a Zephyr driver (`nxp,c40-flash`, 8 KB sectors, 8 byte program unit). In the current
  image `FLASH_Program`, `FLASH_Erase` and their wait loops run from RAM (0x2040xxxx) with interrupts
  locked, so code can write the flash block it runs from.
- No in-tree MCX E31x board uses MCUboot yet: this is a first. Nothing in MCUboot is specific to the SoC
  except the flash driver and the jump to the application.
- The S32K3 boot header (IVT) moves from the application to the bootloader at 0x400000; the application
  builds without it (`BOARD_NXP_MCXE31X_BOOT_HEADER=n`).
- The MPU map of `be_mpu.c` (speculative access hang, ERR011573) goes into the bootloader and the loader
  as a small Zephyr module through sysbuild, not as a patch.
- Image today: `emu_mb` 712 488 B, `emu_sim` 714 640 B.

## Variant 1 (recommended): one slot on the S32K, store on the ESP32

### S32K flash layout (2000 KB usable)

| Partition | Offset | Size | Content |
|---|---|---|---|
| boot_partition | 0x000000 | 64 KB | MCUboot + boot header |
| loader_partition | 0x010000 | 128 KB | firmware loader (Zephyr, `be_link`, flash writer) |
| slot0 | 0x030000 | 1744 KB (0x1B4000) | the application |
| storage (NVS) | 0x1E4000 | 64 KB | **same place as now**: settings survive |
| (HSE firmware) | 0x1F4000 | 48 KB | untouched |

Largest application: about **1.7 MB** (slot minus MCUboot header and trailer), against 712 KB today.
The loader is a separate, small image (estimate 50–80 KB) built from our code; MCUboot itself about
25–35 KB.

### Boot flow

MCUboot starts first after every reset and decides (MCUboot options `SINGLE_APPLICATION_SLOT` and
`BOOT_FIRMWARE_LOADER`):

1. **Loader requested:** the application left a "stay in loader" mark in retained RAM before it
   restarted (`BOOT_FIRMWARE_LOADER_BOOT_MODE`; 256 bytes of the standby RAM at the start of SRAM,
   kept over a functional reset, as `__noinit` data is today) → start the loader.
2. **No valid application:** the SHA-256 of slot 0 does not match its trailer, or the slot is empty
   (`BOOT_FIRMWARE_LOADER_NO_APPLICATION`) → start the loader.
3. **Too many failed starts:** an image access hook of MCUboot (`BOOT_IMAGE_ACCESS_HOOKS`, our code)
   counts starts in retained RAM; the application clears the count when it confirms itself. Three
   starts without a confirm → the hook reports the application as not bootable → loader. This catches
   an image that passes the hash but hangs or resets.
4. Otherwise → start the application.

The loader announces itself on the link (HELLO with role "loader", the reason 1/2/3 and the version
found in slot 0). It configures **no output pins**: the contactor and BMS outputs stay as the reset
left them (output drivers off, the pull-downs on the board hold the drivers off). It sends nothing on
CAN, so a Kona opens its own contactors.

### Update flow

1. The browser sends the new image to the ESP32 (`/miv/api/s32k/ota/*`, 4 KB pieces, as in version 1).
2. **The ESP32 stores it** in its free S32K store slot and checks it there: MCUboot magic, size ≤ the
   S32K slot, board id TLV, SHA-256. Nothing has touched the S32K yet; a bad file stops here.
3. The ESP32 asks the application to enter the loader (`BE_CMD_ENTER_LOADER`). The application refuses
   unless the safety conditions hold (below); otherwise it sets the loader mark and restarts.
4. The loader erases slot 0 and takes the image from the ESP32 block by block (`BE_LINK_OTA` DATA with
   ack, one block in flight), then checks the SHA-256 and restarts.
5. MCUboot finds a valid image and starts it. The application confirms itself (criteria below); the
   ESP32 sees the confirm in HELLO/status and marks its stored copy **current**; the copy before
   becomes **previous**.

Expected time for 712 KB: storing on the ESP32 ≈ 5 s, erase of slot 0 ≈ 89 × 50 ms ≈ 4.5 s (or
erase as the write goes), transfer ≈ 6–8 s → **about 20 s** with no application on the S32K for about
15 s of it.

### Way back: the S32K asks, the ESP32 sends

- **When:** the loader is started for reason 2 (no valid image: power lost during a write, wrong hash)
  or reason 3 (three starts without a confirm: the image hangs, resets or never sees the link).
- **Who decides:** the S32K reports the reason; the ESP32 decides what to send: the **previous**
  copy after a failed new image, the **current** copy after a lost write of an image that had worked.
  It does so on its own, without the browser, and writes an event.
- **Power lost in the middle of the write:** slot 0 is half written; MCUboot sees a wrong hash → loader
  → the ESP32 sends the image again. Until then there is no application; outputs stay off, no CAN.
- **Power lost on the ESP32 side during the transfer:** the same, as soon as the ESP32 is back.

### Without an ESP32

There is no update without the ESP32 anyway (the image comes through it). The risk is narrower: **an
update that fails while the ESP32 is gone, or an ESP32 that dies during the update, leaves the S32K in
the loader**, with outputs off and no CAN, until an ESP32 or SWD brings an image. A running emulator
never depends on the ESP32 (variant A+ holds); only the minute of an update does.

My view: acceptable, because the S32K is then in a safe state (contactors open), the loader says so on
the link and in its LED pattern, and the store on the ESP32 brings it back without a browser as soon as
the ESP32 is there. If that is not acceptable for a site, variant 3 below keeps a copy on the S32K too.

### ESP32 flash

**8 MB (the ESP32 board Dimitar will attach, and the ESP32-S3-WROOM-1-N8R2 of the MIV board):**

| Name | Type | Offset | Size | Note |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB | same as now |
| otadata | data/ota | 0xE000 | 8 KB | same as now |
| app0 | app/ota_0 | 0x10000 | 2304 KB (0x240000) | ESP32 firmware, today 1.95 MB |
| app1 | app/ota_1 | 0x250000 | 2304 KB | |
| coredump | data/coredump | 0x490000 | 64 KB | |
| s32k_a | data/0x40 | 0x4A0000 | 1728 KB (0x1B0000) | S32K image store |
| s32k_b | data/0x40 | 0x650000 | 1728 KB | S32K image store |

- Two S32K copies: **current** and **previous**; a new image always goes into the slot that is not
  current, so the working image is never overwritten by a transfer.
- Each store slot starts with a 4 KB header of our own: magic `S32KIMG1`, header version, image size,
  SHA-256 of the image, version text (from the MCUboot header), state (empty / written / verified /
  current / previous), a counter that orders the two, and a CRC-32 of the header. The image follows at
  4 KB. The image keeps its own MCUboot hash; the ESP32 checks it before and after the transfer.
- Largest S32K image the store takes: 1724 KB, just under the S32K slot (1744 KB). The ESP32 app slots
  have 2.3 MB: room for the ESP32 firmware to grow too.

**4 MB (ESP32ETH_8R8I, `miv_4MB.csv`, stays as it is):** no room for a store. The image is streamed
from the browser through to the loader without a copy. What it loses: the automatic way back (after a
failed image the S32K waits in the loader until a good image is sent again from the page) and the
re-send after a lost write without the browser. Good enough for the bench; not for a site.

## Variant 2: two slots on the S32K (version 1 of this plan)

MCUboot in swap mode with test and confirm: the new image goes to slot 1, MCUboot swaps it in, the image
confirms itself, an image that does not confirm is swapped back at the next start.

| Partition | Offset | Size |
|---|---|---|
| boot_partition | 0x000000 | 64 KB |
| slot0 | 0x010000 | 920 KB |
| spare | 0x0F6000 | 40 KB |
| slot1 | 0x100000 | 912 KB |
| storage (NVS) | 0x1E4000 | 64 KB |

Largest application about **900 KB**. Works without any ESP32 for the way back; the S32K always has a
working application. No changes on the ESP32 flash.

## Variant 3: two slots on the S32K and a copy on the ESP32

Variant 2 plus the store of variant 1 on the 8 MB ESP32. Only gives something over variant 2 when the
copy on the S32K is lost too, which MCUboot's swap already makes unlikely. It keeps the 900 KB limit,
which is what Dimitar wants to avoid. **Not recommended.**

## Comparison

| | Variant 1 | Variant 2 | Variant 3 |
|---|---|---|---|
| Largest S32K image | ~1.7 MB | ~900 KB | ~900 KB |
| Way back without an ESP32 | no | yes | yes |
| S32K without an application during an update | ~15 s, outputs off | never | never |
| Works with the 4 MB ESP32 | yes, no way back | yes | as variant 2 |
| Parts | MCUboot + loader + hook + ESP32 store | MCUboot | all of them |
| Work (days) | 9–11 | 6–9 | 11–13 |

## Rules for every variant (from the review of version 1)

1. **Watchdog from the bootloader on.** MCUboot starts SWT0 through its replaceable
   `mcuboot_watchdog_setup()` / `mcuboot_watchdog_feed()` (our module, SWT0 registers like `be_wdt.c`),
   10 s, fed between the steps of long flash work. It stays on into the loader (which feeds it) and the
   application (`be_wdt_start()` unlocks it and sets its own 500 ms). An image that hangs before
   `be_wdt_start()` is reset after 10 s and counted as a failed start (variant 1) or not confirmed
   (variant 2). Not hard locked; freeze in debug stays on.
2. **Confirm, the same for every configuration** (battery type 0 included): main thread alive, settings
   read from NVS without an error, link to the ESP32 up (HELLO received), 60 s since the start without a
   watchdog or fault reset. Variant 1: the confirm clears the start count and is reported to the ESP32.
   Variant 2: `boot_write_img_confirmed()`. No link within 5 minutes → restart without a confirm
   (variant 2: MCUboot swaps back; variant 1: counts as a failed start). Before an update the S32K
   writes "update to version X pending" to NVS, so the image that runs afterwards knows whether it is
   the new one or the old one back, and reports "update reverted".
3. **Checks of the file** before anything is written: MCUboot magic `0x96f3b83d` (an ESP32 file starts
   with `0xE9`), size within the slot, no load address or encryption flags, vector table pointing into
   the application slot and into RAM, board id TLV (imgtool `--custom-tlv`, tag 0xA0) equal to the
   board's, SHA-256 of the image. The ESP32 checks first and refuses on the page; the S32K (loader or
   application) checks again.
4. **The link is quiet during an update.** The S32K receives with DMA into 2 × 512 bytes; erasing a
   sector locks the interrupts for 40–50 ms. So: one OTA frame in flight, erase only after a whole
   frame is in and before its ack, nothing else from the ESP32 (settings, commands, live battery
   settings, bridge CAN frames wait in the queue of `be_snapshot.cpp`), only the 1 s HELLO; the S32K
   refuses other messages and sends no snapshot or CAN while the update runs.
5. **Web access.** `/miv/api/s32k/ota/*` sits behind the same global middleware as the upstream pages
   and `/ota` (web password, or local network only). The chunk body arrives before the middleware
   decides, so the chunk handler calls `webserver_request_authorized()` itself, as `/ota/upload` does.

## Safety

- The update is refused by the S32K itself unless: the contactor outputs are off (Modbus 226 bits 0–3),
  `contactors_engaged` is 0, the emulator is paused (limits 0) and the current is below 1 A. The page
  says which condition failed.
- From the request on, the core is stopped: no CAN to battery and inverter; outputs off until an
  application runs again.
- Nothing writes NVS during the update; the NVS partition is outside every slot.
- Link silence over 10 s, ABORT or a wrong hash: variant 1 stays in the loader and asks the ESP32
  again; variant 2 leaves slot 1 unmarked and restarts into the old image.

## Programming and tests

- Once per board through SWD: MCUboot, the loader (variant 1) and the application
  (`zephyr.signed.hex`, written to slot 0 as a confirmed image). `flash_pe.sh` takes hex files as it
  takes an ELF; it gets the new paths and an `--app-only` mode.
- Development keeps SWD for speed; sysbuild build directories (`zw/b/emu_mb/emulator/...`) and
  `run_s32k_tests.sh` point at the signed hex of each variant.
- To check in step 1: whether a PEmicro load still wipes the NVS (seen with changed images before). If
  so, the script loads by address range.

## Steps and estimate (variant 1)

| Step | What | Days |
|---|---|---|
| 1 | Partitions; sysbuild with MCUboot (single slot, firmware loader, boot-mode retention, image access hook with the start count); watchdog + MPU module; the loader: `be_link` receiver and flash writer; application without boot header. On the board with SWD: start through MCUboot, enter the loader on request, write slot 0 from a PC script through the loader, three failed starts → loader | 3–4 (+1–2 if MCUboot needs work on this part) |
| 2 | ESP32 8 MB: partition table and environment (the ESP32 board with 8 MB, later the S3), the two store slots with their header, the transfer to the loader, the way back on its own; a PC script through the ESP32 | 3 |
| 3 | `/miv/api/s32k/ota/*` and the second field "S32K firmware" in the Update view, progress, state of the store (current / previous versions) | 1 |
| 4 | On the boards: full cycle; wrong file, ESP32 file and other board id refused; image that hangs in its first lines → back to previous; image with a broken link → back; power off during the write (S32K and ESP32 separately); refused with contactors closed and without the web password; the 4 MB board without a store | 2 |
| | **Total** | **about 9–11 working days** |

A commit (no push) after every step, with measurements.

## Not included

- Production signing keys (decision 1); updating MCUboot or the loader over the link (SWD only);
  encrypted images, the HSE, downgrade protection; updating the S32K over Modbus/RS485 without an
  ESP32; updating the ESP32 from the S32K.

## Decisions for Dimitar

1. **Signature:** SHA-256 now (protects against broken transfers, not against someone who can reach the
   page); ECDSA P-256 with the key outside the public repository before the first delivery. GPL-3.0:
   if the product refuses images not signed by MIV, users must get a way to install their own builds.
2. **Variant 1** with its condition: without an ESP32 a failed update leaves the S32K waiting in the
   loader (outputs off). Or variant 2 with the 900 KB limit and no such condition.
3. **The 8 MB ESP32 board** for the bench: which board is it (module, pins of the W5500 and of the UART
   to the S32K)? It needs its own environment until the MIV board with the S3 exists.
