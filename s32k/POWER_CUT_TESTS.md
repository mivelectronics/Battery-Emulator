# Power-cut tests of the S32K update (OTA_PLAN.md, step 4)

Real power cuts by hand, announced before they run. FRDM-A-S32K312 (S32K) and ProDINo ESP32 Ethernet v15
(ESP32) linked as in `wiring/WIRING.md`; the S32K is updated from the `/miv/update` page.

## Set-up

- S32K: `tools/build_ota.sh prodslow` (TEST ONLY): the production application with a MCUboot that logs on
  the console and slows every swap step down (`s32k/modules/miv_boot/slow_swap.conf`, 9 ms per step).
  A swap of a 547 KB image then takes about 41 s instead of 4 s. LEDs of the FRDM: blue = MCUboot runs,
  **red blinking = MCUboot swaps** (an update or a swap back), green blinking = the application runs.
  A swap that MCUboot resumes after a power loss is not slowed and does not blink red: it takes about 4 s.
- Images to send: `41_battery_emulator/ota_test_images/s32k_app_<version>.bin` (outside the repo, signed
  for this board), a different version for each update.
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

## Results

### Test 1 by a STAND-IN (11.11.2026): a debugger reset, the power was NOT cut

`tools/ota_reset_test.py <esp32 ip> <app_V.bin> <percent> [hold s]` sends the image as the page does and, at
<percent> of the bytes, starts the PEmicro debugger server: it halts the S32K core as it connects to the chip;
after [hold s] gdb resets the core into a run (`monitor _reset`, detach). 0.6.8 confirmed before, 0.7.0 sent.

| Run | S32K halted at | Reset after | ESP32 / what the page gets | S32K after the reset |
|---|---|---|---|---|
| a | 159 000 of 547 190 bytes (29 %) | 3.5 s | failed, -208 "transfer out of step"; **the next pieces got 503 for ever** (bug, below) | MCUboot: swap type none, 0.6.8, confirmed |
| b (fixed ESP32) | 187 000 (34 %) | 3.6 s | the next piece: 409, -208 "transfer out of step"; failed | 0.6.8 confirmed, no swap |
| c (fixed ESP32) | 242 000 (44 %) | 11.6 s | after 5 s without an answer: 409, -103 "the S32K stopped answering"; failed | 0.6.8 confirmed, no swap |

In all three: MCUboot "Secondary image: magic=unset", "Swap type: none", "image 0.6.8: confirmed", no test image;
the copy the ESP32 was writing (store slot 0) is empty again, slot 1 keeps 0.6.8 "current". Then 0.7.0 from
`ota_web_send.py` all the way: transfer 10.9 s, swap 41.0 s (3 949 steps), "test image, not confirmed", confirmed
60 s after its start, "installed"; the ESP32 store: 0.7.0 current, 0.6.8 previous.
Log: `41_battery_emulator/power_tests/test1_reset_20261111.log` (outside the repo).

Bug found and fixed (ESP32, `miv_web.cpp`): after a failed transfer the buffer stays full, and a piece was
answered 503 ("send it again") instead of the error; the page repeats a 503 for ever, so it would have shown
"Sending…" with no end. Now 503 only while the transfer runs; otherwise the piece gets the error (409).

What this stand-in does NOT cover, against a real power cut:
- the supply going down: brown-out, the flash supply falling during a program or erase operation, a write
  half done in the flash cells. A debugger halt stops the core between instructions; a program or erase
  already started by the flash controller completes while the core is halted, before the reset, so no torn
  write is tested;
- the OpenSDA USB going away: the console port and the link side of the FRDM stay powered;
- the time a real cut lasts and the power-on reset path (POR, the clocks and the PLL from cold), only a
  debugger (functional) reset;
- the ESP32 side staying up in all three runs (tests 2 and 6 cut the ESP32).
In this test (the transfer) the S32K writes slot 1 only, and MCUboot ignores slot 1 without the trailer magic,
so a torn write there could not do more than the halt did; the real cut matters most for tests 3–5 (the swap).
