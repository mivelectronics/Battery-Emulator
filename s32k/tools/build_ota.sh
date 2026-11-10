#!/bin/bash
# Sysbuild build of the emulator under MCUboot (OTA_PLAN.md): MCUboot + the application in slot 0.
#   build_ota.sh <variant> [extra west args]
# variant: mb   = no simulator, Modbus on the OpenSDA port (like zw/b/emu_mb)
#          prod = production (like zw/b/emu)
#          sim  = bench simulator, Modbus on the OpenSDA port (like zw/b/emu_sim)
# Output in $ZW/b/ota_<variant>: mcuboot/zephyr/zephyr.elf (bootloader, with the boot header) and
# emulator/zephyr/zephyr.signed.hex / .signed.bin (the application image for slot 0 or slot 1).
set -eu
VARIANT="${1:?variant: mb, prod or sim}"
shift
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/zenv.sh"
S32K="$(cd "$HERE/.." && pwd)"
APP="$S32K/app/emulator"

OVL="mcuboot_app.overlay"
CONF="mcuboot_app.conf"
SIM=0
case "$VARIANT" in
  mb) OVL="$OVL;modbus_on_opensda.overlay"; CONF="$CONF;modbus_on_opensda.conf" ;;
  prod) ;;
  sim) OVL="$OVL;modbus_on_opensda.overlay"; CONF="$CONF;modbus_on_opensda.conf"; SIM=1 ;;
  *) echo "unknown variant $VARIANT"; exit 2 ;;
esac

west build --sysbuild -b frdm_a_s32k312 -d "$ZW/b/ota_$VARIANT" "$APP" "$@" -- \
  -DBOARD_ROOT="$S32K" \
  -DEXTRA_ZEPHYR_MODULES="$S32K/modules/miv_boot" \
  -Demulator_BE_SIM=$SIM \
  "-Demulator_EXTRA_DTC_OVERLAY_FILE=$OVL" \
  "-Demulator_EXTRA_CONF_FILE=$CONF"
