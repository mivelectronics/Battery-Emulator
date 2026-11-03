#!/bin/bash
# Zephyr v4.5.0 workspace + SDK 1.0.1 (arm) inside the project folder. Re-runnable.
set -e
# long paths for hal_nxp without touching the global git config
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.longpaths GIT_CONFIG_VALUE_0=true
# Workspace next to the repository clone (override with BE_WORKDIR)
BASE="${BE_WORKDIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"
ZW="$BASE/zw"
SDKDIR="$BASE/sdk"
mkdir -p "$ZW" "$SDKDIR"
cd "$ZW"
if [ ! -x .venv/Scripts/python.exe ]; then python -m venv .venv; fi
PY="$ZW/.venv/Scripts/python.exe"
"$PY" -m pip install -q --upgrade pip
"$PY" -m pip install -q west
WEST="$ZW/.venv/Scripts/west.exe"
if [ ! -d .west ]; then "$WEST" init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 .; fi
"$WEST" config manifest.project-filter -- "-.*,+hal_nxp,+cmsis,+cmsis_6,+mcuboot,+mbedtls"
"$WEST" update --narrow -o=--depth=1
"$PY" -m pip install -q -r zephyr/scripts/requirements-base.txt
"$PY" -m pip install -q -r zephyr/scripts/requirements-build-test.txt || true
cd "$SDKDIR"
for f in zephyr-sdk-1.0.1_windows-x86_64_minimal.7z toolchain_gnu_windows-x86_64_arm-zephyr-eabi.7z; do
  [ -f "$f" ] || curl -sSL -o "$f" "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/$f"
done
[ -d zephyr-sdk-1.0.1 ] || "/c/Program Files/7-Zip/7z.exe" x -y zephyr-sdk-1.0.1_windows-x86_64_minimal.7z > /dev/null
cd zephyr-sdk-1.0.1
ls
"/c/Program Files/7-Zip/7z.exe" x -y ../toolchain_gnu_windows-x86_64_arm-zephyr-eabi.7z > /dev/null
ls
echo SETUP_DONE
