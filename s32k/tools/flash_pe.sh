#!/bin/bash
# Flash a Zephyr ELF to the FRDM-A-S32K312 through the on-board PEmicro OpenSDA
# (Embedded Multilink). Usage: tools/flash_pe.sh path/to/zephyr.elf [device]
# Starts pegdbserver, loads the image with gdb, resets and runs, stops the server.
set -u
ELF="$1"
DEV="${2:-NXP_S32K3xx_S32K312}"
PE="/c/PEMicro/gdbserver"
GDB="$PE/gdb/arm-none-eabi-gdb.exe"
PORT=7224
LOG="$(dirname "$0")/flash_pe.log"
HWPORT=$("$PE/pegdbserver_console.exe" -showhardware 2>/dev/null | grep -o "PORT=PEM[0-9A-F]*" | head -1 | cut -d= -f2)
[ -n "$HWPORT" ] || { echo "no PEmicro interface found"; exit 2; }

"$PE/pegdbserver_console.exe" -startserver -device=$DEV -interface=USBMULTILINK -port=$HWPORT \
  -serverport=$PORT -singlesession > "$LOG" 2>&1 &
SRV=$!
for i in $(seq 1 30); do grep -q "All Servers Running" "$LOG" && break; sleep 0.5; done
grep -q "All Servers Running" "$LOG" || { echo "pegdbserver did not start:"; tail -20 "$LOG"; kill $SRV 2>/dev/null; exit 3; }

WELF=$(cygpath -m "$ELF")
"$GDB" -q -batch \
  -ex "set pagination off" \
  -ex "target remote localhost:$PORT" \
  -ex "monitor _reset" \
  -ex "load" \
  -ex "monitor _reset" \
  -ex "detach" \
  "$WELF" 2>&1 | grep -v "^warning: " | tail -15
RC=${PIPESTATUS[0]}
sleep 1
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
exit $RC
