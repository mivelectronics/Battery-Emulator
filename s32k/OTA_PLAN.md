# S32K312 firmware update over the web page (plan, 09.11.2026)

Goal: update the S32K application from the `/miv/update` page of the ESP32, without SWD, with a way back
when the new image does not come up. SWD stays the way to program the bootloader once.

Nothing here is built yet. Facts below were checked in the workspace (`zw/`, Zephyr v4.5.0) and in the
current build (`zw/b/emu_mb`).

## a) Bootloader: MCUboot

**Choice: MCUboot**, built by sysbuild next to the application, in swap mode with test and confirm.

What makes it work without patches:

- MCUboot is in the workspace (`zw/bootloader/mcuboot`), with mbedTLS; `west` is in `zw/.venv`, so
  sysbuild (`west build --sysbuild`, `SB_CONFIG_BOOTLOADER_MCUBOOT=y`) is available.
- The flash has a Zephyr driver: `nxp,c40-flash` (`drivers/flash/flash_mcux_c40.c`), 8 KB erase
  sectors, 8 byte program unit. NVS already uses it.
- **Program and erase run from RAM:** in the current image `FLASH_Program`, `FLASH_Erase` and the
  start-and-wait loops of the C40 HAL sit at 0x2040xxxx (`CONFIG_CODE_DATA_RELOCATION`), with interrupts
  locked. So code running from a block can write the same block. This is what a swap needs: MCUboot
  runs from block 0 and rewrites the primary slot in block 0.
- MCUboot only needs flash partitions in the devicetree: `boot_partition`, `slot0_partition`,
  `slot1_partition`. They go into our own board (`s32k/boards/nxp/frdm_a_s32k312`), so `zw/zephyr` and
  `zw/bootloader` stay untouched. MCUboot settings go into `sysbuild/mcuboot.conf` and
  `sysbuild/mcuboot.overlay` of the application.
- No in-tree board of the MCX E31x family uses MCUboot yet, so this is a first. Nothing in MCUboot is
  specific to the SoC except the flash driver and the jump to the application (Cortex-M7, generic).

Mode: **swap using move** (or swap using offset, both in this MCUboot): the new image goes to slot 1 as
a test image; MCUboot swaps it in at the next start; the application confirms itself
(`boot_write_img_confirmed()`) once it is healthy. A start that does not confirm (reset, watchdog, power
loss) makes MCUboot swap the old image back. MCUboot resumes an interrupted swap after a power loss.

Image check: **SHA-256 in the image trailer from the start** (`BOOT_SIGNATURE_TYPE_NONE`), **ECDSA P-256
signature as a later step** (decision 1 below).

Risks found, with what to do about them:

1. **Boot header (IVT).** Today the application carries the S32K3 boot header at 0x400000
   (`BOARD_NXP_MCXE31X_BOOT_HEADER=y`, code partition from 0x100). With MCUboot the boot header goes with
   the bootloader at 0x400000, and the application turns it off (`=n` in its configuration).
2. **Speculative access hang (ERR011573, `be_mpu.c`).** The bootloader is a different binary and can
   hit the same stop. The MPU map of `be_mpu.c` goes into the bootloader as a small Zephyr module
   (`SYS_INIT`) added to the MCUboot image through sysbuild, not a patch of MCUboot.
3. **Resets.** Destructive resets left this chip dead before (FIRMWARE_PLAN section 6); the application
   already uses functional resets, and MCUboot only runs after them. To check on the board in step 1.
4. **Console.** MCUboot logs on LPUART6, which is the Modbus port of the `emu_mb` test build: its log
   is turned off in that variant.

**Watchdog from the bootloader on.** A test image that hangs before it starts SWT0 would never reset,
so there would be no way back. Therefore MCUboot starts SWT0 itself and it stays on into the
application:

- MCUboot lets the watchdog be replaced through two weak functions, `mcuboot_watchdog_setup()` and
  `mcuboot_watchdog_feed()` (`BOOT_WATCHDOG_SETUP_AT_BOOT`, `BOOT_WATCHDOG_FEED`). Our small module (the
  same one that carries the MPU map) implements them on the SWT0 registers, like `be_wdt.c` does, not
  through the Zephyr SWT driver.
- Timeout **10 s**, fed by MCUboot between the sector steps of a swap (`MCUBOOT_WATCHDOG_FEED()` is
  called there), so a swap of any length is covered, and a swap that stops is reset.
- The application finds SWT0 already running: `be_wdt_start()` unlocks it, sets its own 500 ms and its
  two stages as today. Between the jump and `be_wdt_start()` the 10 s of the bootloader apply. A hang in
  that time resets the chip, the image is not confirmed, MCUboot swaps the old one back.
- Not locked (no hard lock), so the application can reconfigure it. The freeze-in-debug bit stays set
  as today, so SWD debugging still works.

Fallback if MCUboot fails on this part: an own bootloader of about 300 lines with the same layout
(length + magic + CRC32/SHA-256, then jump). "Pick the slot" without a swap needs every image built
twice, once for each slot address; "copy slot 1 over slot 0" is simple but has no way back. Only if
step 1 shows a blocker.

## b) Flash layout (2000 KB usable, two blocks)

Code flash: block 0 = 0x400000–0x4FFFFF (1024 KB), block 1 = 0x500000–0x5F3FFF (976 KB usable; the last
48 KB belong to the HSE firmware and stay untouched).

| Partition | Offset | Size | Block | Now |
|---|---|---|---|---|
| boot_partition (MCUboot + boot header) | 0x000000 | 64 KB | 0 | IVT 0x100 + code |
| slot0 (running image) | 0x010000 | 920 KB (0xE6000) | 0 | code |
| spare | 0x0F6000 | 40 KB | 0 | code |
| slot1 (new image) | 0x100000 | 912 KB (0xE4000) | 1 | code |
| storage (NVS) | 0x1E4000 | 64 KB | 1 | **same as now** |

- Swap using move needs the primary slot one sector bigger than the secondary: 920 = 912 + 8 KB.
- **Largest image: about 900 KB** (912 KB minus header and trailer). Today: `emu_mb` 712 488 B,
  `emu_sim` 714 640 B → **about 190 KB (26 %) of room**. The code partition today is 1936 KB, so the
  image limit drops to less than half: that is the price of a way back.
- The NVS partition does not move, so the settings survive the update. MCUboot never touches it.
- 115 and 114 sectors in the two slots: within MCUboot's default of 128 (`BOOT_MAX_IMG_SECTORS`).
- MCUboot itself: about 25–35 KB with SHA-256 only, about 45 KB with ECDSA; 64 KB leaves room.

## c) Path of the image: browser → ESP32 → S32K

**Room on the ESP32:** none to spare. The only free space is the inactive OTA slot of the ESP32
(0x1F0000), and it is the rollback image of the ESP32 itself (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).
**So: streaming, no copy on the ESP32.**

1. The `/miv/update` view gets a second field, "S32K firmware" (`zephyr.signed.bin`). The browser
   reads the file, takes its SHA-256 and sends it in 4 KB pieces:
   - `POST /miv/api/s32k/ota/begin` (size, SHA-256, version text) → the ESP32 checks the conditions of d)
     and sends BEGIN over the link;
   - `POST /miv/api/s32k/ota/chunk?offset=N` (4 KB body) → accepted when the previous piece is confirmed
     by the S32K, else 409 with the confirmed offset: the browser goes on from there;
   - `GET /miv/api/s32k/ota/state` → state, confirmed bytes, error text (the progress bar);
   - `POST /miv/api/s32k/ota/finish` → the S32K checks the SHA-256 of the whole slot, marks it as a test
     image and restarts after 2 s.
2. Link (`be_link.h`): the message type `BE_LINK_OTA = 9` is already reserved. Sub-messages BEGIN,
   DATA (offset + up to 1000 bytes; the frame already has a CRC-16), FINISH, ABORT. The S32K answers
   every one with `BE_LINK_ACK` (status + offset). The ESP32 sends the next block only after the ack and
   repeats a block that was not acknowledged within 200 ms (like the settings today).
3. On the S32K, in the main thread: erase a sector when the write reaches it, write, read back. Every
   sector erase locks the interrupts for 40–50 ms (measured with NVS), so the protocol must never have
   more than one block in flight: the link RX buffer then never overflows.
4. Expected time for 712 KB: erase 89 × ~50 ms ≈ 4.5 s, transfer at 2 Mbit/s with one block in flight
   ≈ 6–8 s, HTTP pieces ≈ 3 s → **about 15–20 s**, then the swap at start (estimate 5–10 s, measured in
   step 1).
5. **Confirming the new image, the same for every configuration** (battery type 0 included, where the
   core does not run). The image confirms itself when all of these hold:
   - the main thread is alive (it feeds the watchdog);
   - the settings were read from NVS without an error;
   - the link to the ESP32 is up (HELLO received from it);
   - 60 s have passed since the start without a watchdog or fault reset.

   The link is in the list on purpose: an image with a broken link could never be updated again over
   the web. Until it is confirmed the status shows "S32K: test image, not confirmed". `s32k_fw` in
   `/miv/api/status` shows the version (the version text grows a build id).
6. **No ESP32 after the restart.** If the link does not come up within **5 minutes**, the new image
   restarts itself without confirming, and MCUboot puts the old image back. Before BEGIN the S32K
   writes "update to version X pending" into NVS; the old image, finding that marker with its own,
   different version, reports "update reverted" (event and status) and clears it. An update is always
   started from the ESP32, so the ESP32 was there a minute before; if it is really gone, running the
   proven old image is the safe side.
7. **BEGIN carries the first 32 bytes of the image** (the MCUboot header). The S32K refuses the update
   unless:
   - the magic is MCUboot's (`0x96f3b83d`): an ESP32 file starts with `0xE9` and fails here;
   - header + image + trailer fit in slot 1 (912 KB);
   - the image has no load address and no encryption flags.

   At FINISH, before the image is marked, more checks that cost nothing:
   - the vector table at the start of the image points into slot 0 (reset handler) and into RAM
     (stack);
   - a board id TLV (imgtool `--custom-tlv`, our own tag 0xA0) equals the board's id, for example
     `frdm_a_s32k312` or later the MIV board: an image of another board is refused;
   - the SHA-256 of the whole image matches the hash TLV.

   The ESP32 makes the first check too, before it sends anything, so a wrong file is refused at once on
   the page.
8. **The link is quiet during an update, apart from the update itself.** The S32K receives with DMA into
   2 × 512 bytes; while a sector is erased the interrupts are locked for 40–50 ms and only about
   1 KB more can come in. So:
   - the ESP32 sends exactly one OTA frame and waits for its ack;
   - the S32K erases only after the whole frame is in and before it acks, so no frame of the update is
     on the wire during an erase;
   - the ESP32 holds back everything else: settings, commands, the live battery settings, and the CAN
     frames of bridge mode. The queue (`out_pump` in `be_snapshot.cpp`) waits and goes on after the
     update. Only the 1 s HELLO (under 40 bytes) still goes out;
   - the S32K refuses any other message while an update runs, and stops its own snapshot and CAN
     traffic (the core is stopped).

The same commands are reachable without the web page (step 2): a small script on the PC through the
ESP32, so the S32K side is tested before the page exists.

## d) Safety

- BEGIN is refused unless: the contactor outputs of the S32K are all off (Modbus 226 bits 0–3 = 0),
  `contactors_engaged` is 0, the emulator is paused (charge and discharge limits 0) and the measured
  current is below 1 A. The page says which condition failed.
- On BEGIN the S32K stops the core: no CAN to the battery and inverter. A Kona opens its own contactors
  when the vehicle frames stop. The outputs stay off until the restart.
- The watchdog stays on. While an update runs it only asks for the main thread (the core is stopped on
  purpose); erases are one sector at a time, 40–50 ms, well under its 500 ms.
- No writes to NVS during the update; the NVS partition is outside both slots.
- Any link silence over 10 s, ABORT, or a wrong SHA-256 at FINISH: slot 1 is left unmarked, the S32K
  restarts into the old image with the core as configured.
- Never a write while the contactors are closed: the check is on the S32K itself, not only in the page.
- **Web access.** `/miv/api/s32k/ota/*` sits behind the same global middleware of the server as the
  upstream pages and `/ota`: the web password when it is on, otherwise local network only. One trap,
  known from ElegantOTA: the body of an upload arrives in the body callback before the middleware
  decides. So the chunk handler checks `webserver_request_authorized()` itself before it passes a single
  byte on, as `/ota/upload` does, and the request is then answered 401/403 by the middleware.

## e) Programming and tests with the new layout

- Once per board through SWD: the bootloader (`build/mcuboot/zephyr/zephyr.elf`, has the boot header)
  and the application (`build/emulator/zephyr/zephyr.signed.hex`). `flash_pe.sh` takes the hex as it
  takes an ELF (gdb `load`), so it only needs the new path; a `--app-only` call loads only slot 0.
- Development keeps SWD for speed: build directories become sysbuild ones (`zw/b/emu_mb/emulator/...`),
  `run_s32k_tests.sh` points at the signed hex of each variant. A SWD load writes slot 0 directly and
  marks it confirmed (`--confirm` of imgtool), so no swap happens after it.
- To check in step 1: whether a PEmicro load still wipes the NVS (seen before with changed images). If
  so, the script loads by address range.

## Steps and estimate

| Step | What | Days |
|---|---|---|
| 1 | Partitions in the board, sysbuild with MCUboot, MPU module in the bootloader, application without boot header; on the board: start through MCUboot, an image put into slot 1 through SWD swaps in, without confirm swaps back, settings survive | 2–3 (+1–2 if MCUboot needs work on this part) |
| 2 | Link messages BEGIN/DATA/FINISH/ABORT with acks, writer on the S32K, checks of d), confirm logic, a PC script through the ESP32 | 2–3 |
| 3 | `/miv/api/s32k/ota/*` on the ESP32 and the second field in the Update view, progress | 1 |
| 4 | On the boards: full cycle; broken image (wrong hash, ESP32 file, image of another board id) refused; image that hangs in its very first lines (before `be_wdt_start`) → reset by the bootloader's watchdog and back to the old one; image with a broken link → not confirmed, back after 5 min; ESP32 off after the restart; power off during transfer and during the swap; update refused with contactors closed and without the web password | 1–2 |
| | **Total** | **about 6–9 working days** |

A commit (no push) after every step, with measurements.

## Not included

- Production signing keys and their storage (decision 1).
- Updating MCUboot itself over the link (stays SWD).
- Encrypted images, the HSE, downgrade protection.
- Updating the ESP32 from the S32K, or the S32K over Modbus/RS485 without the ESP32.
- The new MIV board (TJA1448A, ESP32-S3): the same plan applies, its layout is checked when the board
  exists.

## Decisions for Dimitar

1. **Signature.** SHA-256 only is enough against broken transfers, not against someone who can reach
   the web page. ECDSA P-256 needs a private key kept out of the public repository. The firmware is
   GPL-3.0: if the product refuses images not signed by MIV, GPL-3.0 asks to give users a way to install
   their own builds (their own key or an unlock). Proposal: SHA-256 now, ECDSA with a documented way for
   users before the first delivery.
2. **Image limit of about 900 KB** instead of 1936 KB. Enough for now (712 KB); the new board's S32K
   has the same flash.
