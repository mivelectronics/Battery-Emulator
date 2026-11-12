#!/bin/bash
# Sysbuild build of the emulator under MCUboot (OTA_PLAN.md): MCUboot + the application in slot 0.
#   build_ota.sh <variant> [extra west args]
# variant: mb   = no simulator, Modbus on the OpenSDA port (like zw/b/emu_mb)
#          prod = production (like zw/b/emu)
#          sim  = bench simulator, Modbus on the OpenSDA port (like zw/b/emu_sim)
#          link = the link on the OpenSDA port at 115200, a PC in place of the ESP32 (ota_link_test.py);
#                 core off (battery type 0), so an update may start: the whole update cycle
#          linksim = the same with the bench simulator: the core runs, the refusals of BEGIN (its battery
#                 reports 5 A, so no update starts on it)
#          prodslow = TEST ONLY, never a release: prod with a MCUboot that logs on the console and swaps
#                 slowly with the red LED blinking (s32k/modules/miv_boot/slow_swap.conf), for cutting the
#                 power in the middle of a swap by hand
# Board: frdm_a_s32k312, or another one with BOARD=<board> (miv_be_main); the image carries the board name.
# Output in $ZW/b/ota_<variant> (FRDM) or $ZW/b/ota_<board>_<variant>: mcuboot/zephyr/zephyr.elf (bootloader,
# with the boot header) and emulator/zephyr/zephyr.signed.hex / .signed.bin (the application image for slot 0
# or slot 1).
set -eu
VARIANT="${1:?variant: mb, prod, sim, link, linksim or prodslow}"
shift
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/zenv.sh"
S32K="$(cd "$HERE/.." && pwd)"
APP="$S32K/app/emulator"

OVL="mcuboot_app.overlay"
CONF="mcuboot_app.conf"
MCUBOOT_CONF=()
# The simulator comes in as CONFIG_BE_SIM (sim.conf): sysbuild does not pass other CMake variables on
case "$VARIANT" in
  mb) OVL="$OVL;modbus_on_opensda.overlay"; CONF="$CONF;modbus_on_opensda.conf" ;;
  prod) ;;
  sim) OVL="$OVL;modbus_on_opensda.overlay"; CONF="$CONF;modbus_on_opensda.conf;sim.conf" ;;
  link) OVL="$OVL;link_on_opensda.overlay"; CONF="$CONF;link_on_opensda.conf" ;;
  linksim) OVL="$OVL;link_on_opensda.overlay"; CONF="$CONF;link_on_opensda.conf;sim.conf" ;;
  # An extra fragment for MCUboot replaces sysbuild/mcuboot.conf, so both are named
  prodslow) MCUBOOT_CONF=("-Dmcuboot_EXTRA_CONF_FILE=$(cygpath -m "$APP/sysbuild/mcuboot.conf");$(cygpath -m "$S32K/modules/miv_boot/slow_swap.conf")") ;;
  *) echo "unknown variant $VARIANT"; exit 2 ;;
esac

BOARD="${BOARD:-frdm_a_s32k312}"
OUT="$ZW/b/ota_$VARIANT"
[ "$BOARD" = frdm_a_s32k312 ] || OUT="$ZW/b/ota_${BOARD}_$VARIANT"

west build --sysbuild -b "$BOARD" -d "$OUT" "$APP" "$@" -- \
  -DBOARD_ROOT="$S32K" \
  -DEXTRA_ZEPHYR_MODULES="$S32K/modules/miv_boot" \
  "-Demulator_EXTRA_DTC_OVERLAY_FILE=$OVL" \
  "-Demulator_EXTRA_CONF_FILE=$CONF" \
  "${MCUBOOT_CONF[@]}"
