# Power-cut tests of the S32K update (OTA_PLAN.md, step 4)

Real power cuts by hand, announced before they run. FRDM-A-S32K312 (S32K) and ProDINo ESP32 Ethernet v15
(ESP32) linked as in `wiring/WIRING.md`; the S32K is updated from the `/miv/update` page.

## Set-up

- S32K: `tools/build_ota.sh prodslow` (TEST ONLY): the production application with a MCUboot that logs on
  the console and slows every swap step down (`s32k/modules/miv_boot/slow_swap.conf`, 9 ms per step).
  A swap of a 547 KB image then takes about 41 s instead of 4 s. LEDs of the FRDM: blue = MCUboot runs,
  **red blinking = MCUboot swaps** (an update or a swap back), green blinking = the application runs.
  A swap that MCUboot resumes after a power loss is not slowed and does not blink red: it takes about 4 s.
- Images to send: `41_battery_emulator/ota_test_images/s32k_app_<version>.bin` (outside the repo) (signed for this board), a different version for
  each update.
- Timeline: `s32k/tools/power_test_log.py COM13 <esp32 ip> <seconds> <log>` logs the S32K console (MCUboot
  and the application, the port is reopened after a cut) and what the ESP32 reports (image, test image,
  last update, the transfer).
- Measured without a cut (11.11.2026): transfer 10.7 s (with the copy on the ESP32), swap 41.0 s, the new
  image confirmed 60 s after its start; a debugger reset of a test image (a stand-in for a cut) gave a slow
  swap back of 41.0 s and "reverted".

## Tests

| # | Cut | When | Expected after the power is back |
|---|---|---|---|
| 1 | S32K | during the transfer (page progress 30–70 %) | page: "the S32K stopped answering"; S32K starts its old image, confirmed, nothing marked; a new update works |
| 2 | ESP32 | during the transfer | S32K: no data for 10 s → restart into the old image; ESP32 back: old image, the slot it was writing is empty |
| 3 | S32K | during the swap (red LED blinking, 10–30 s into it) | MCUboot resumes the swap (not slowed, no red blinking), starts the new image on test, it confirms after 60 s; page: installed |
| 4 | S32K | the new image runs on test (green LED, within its first 60 s) | MCUboot swaps the old image back (red blinking 41 s); old image confirmed; page: did not confirm |
| 5 | S32K twice | as 4, then again during that swap back (red blinking) | MCUboot resumes the swap back; old image confirmed; page: did not confirm |
| 6 | both | during the transfer | as 1 and 2: old image on the S32K, a new update works |

After each test: the S32K image and its confirm (console and ESP32 status), the update result on the page,
the copies on the ESP32 (`/miv/api/s32k/store`), and one update from the page that works.
