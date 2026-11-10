"""Images of a sysbuild build under MCUboot (tools/build_ota.sh), for loading through SWD and for tests.

    python ota_images.py <build dir> full  [--slot1-version V]  -> full.hex: MCUboot + slot 0 (+ slot 1)
    python ota_images.py <build dir> slot1 --slot1-version V     -> slot1.hex: a test image in slot 1 only
    python ota_images.py <build dir> bin   --version V           -> app_<V>.bin: the image to send over OTA

Every image carries the board id (TLV 0xA0, CONFIG_BOARD of the build); --board signs for another board.

slot 0 gets the application without a trailer (the application confirms such an image itself); with
--slot1-version the same application goes to slot 1 as well, signed with that version and marked for a
test start (imgtool --pad), so that MCUboot swaps it in at the next start. Run with the Python of zw/.venv
(imgtool needs cryptography).
"""
import argparse
import os
import subprocess
import sys

from intelhex import IntelHex

FLASH = 0x400000
SLOT0 = FLASH + 0x010000
SLOT1 = FLASH + 0x100000
SLOT_SIZE = 912 * 1024  # the smaller slot, as sysbuild signs with
HEADER = 0x1000
BOARD_TLV = 0xA0
IMGTOOL = os.path.join(os.path.dirname(__file__), "..", "..", "..", "zw", "bootloader", "mcuboot", "scripts",
                       "imgtool.py")


def board_of(build):
    """CONFIG_BOARD of the application: the board id the S32K checks in an update (TLV 0xA0)"""
    with open(os.path.join(build, "emulator", "zephyr", ".config"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("CONFIG_BOARD="):
                return line.split("=", 1)[1].strip().strip('"')
    raise SystemExit("CONFIG_BOARD not found in the build")


def sign(app_bin, out, version, addr=None, test=False, board=None):
    cmd = [sys.executable, IMGTOOL, "sign", "--version", version, "--header-size", hex(HEADER), "--slot-size",
           str(SLOT_SIZE), "--align", "8"]
    if board:
        cmd += ["--custom-tlv", hex(BOARD_TLV), board]
    if test:
        cmd.append("--pad")
    if addr is not None:
        cmd += ["--hex-addr", hex(addr)]
    subprocess.run(cmd + [app_bin, out], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("what", choices=["full", "slot1", "bin"])
    ap.add_argument("--slot1-version")
    ap.add_argument("--version", default="0.0.0+0")
    ap.add_argument("--board", help="board id in the image (default: the build's CONFIG_BOARD; another one tests"
                                       " the refusal)")
    a = ap.parse_args()
    board = a.board or board_of(a.build)
    app_bin = os.path.join(a.build, "emulator", "zephyr", "zephyr.bin")
    boot_bin = os.path.join(a.build, "mcuboot", "zephyr", "zephyr.bin")
    out_dir = a.build

    if a.what == "bin":
        out = os.path.join(out_dir, f"app_{a.version}.bin")
        sign(app_bin, out, a.version, board=board)
        print(out)
        return
    if a.what == "slot1":
        out = os.path.join(out_dir, "slot1.hex")
        sign(app_bin, out, a.slot1_version, SLOT1, test=True, board=board)
        print(out)
        return

    full = IntelHex()
    full.loadbin(boot_bin, offset=FLASH)
    slot0 = os.path.join(out_dir, "slot0.hex")
    sign(app_bin, slot0, a.version, SLOT0, board=board)
    full.merge(IntelHex(slot0), overlap="error")
    if a.slot1_version:
        slot1 = os.path.join(out_dir, "slot1.hex")
        sign(app_bin, slot1, a.slot1_version, SLOT1, test=True, board=board)
        full.merge(IntelHex(slot1), overlap="error")
    out = os.path.join(out_dir, "full.hex")
    full.write_hex_file(out)
    print(out, "segments:", [(hex(s), hex(e)) for s, e in full.segments()])


if __name__ == "__main__":
    main()
