"""Image update over the link, on the FRDM with a "link" variant: the PC takes the place of the ESP32 on the
OpenSDA port and sends images as the ESP32 does (one frame in flight, repeated until acknowledged).

    python ota_link_test.py COM13 <zw/b/ota_link> [--baud 115200] [--revert]     (build_ota.sh link)
    python ota_link_test.py COM13 <zw/b/ota_linksim>                              (build_ota.sh linksim)

Images are signed from the build (ota_images.py): 0.6.1 (good), 0.6.2 for another board, 0.6.3 (revert test).
With the core off (link): refusals at BEGIN (not an image, too big), a lost ack and a wrong offset, the update
to 0.6.1 (swap, test image, confirm), a corrupted image (hash), an image for another board, an abort, a stalled
transfer; with --revert an update that is not confirmed (HELLO stopped) and comes back as reverted.
With the core running (linksim): the safety refusals (not paused; paused but 5 A from the simulated battery).
Run with the Python of zw/.venv.
"""
import argparse
import os
import subprocess
import sys
import time

import be_link as bl

HERE = os.path.dirname(os.path.abspath(__file__))
results = []
t0 = time.time()


def log(msg):
    print(f"{time.time() - t0:7.1f} {msg}", flush=True)


def check(ok, what):
    results.append((ok, what))
    log(("PASS " if ok else "FAIL ") + what)
    return ok


def sign(build, version, board=None):
    cmd = [sys.executable, os.path.join(HERE, "ota_images.py"), build, "bin", "--version", version]
    if board:
        cmd += ["--board", board]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip().splitlines()[-1]
    with open(out, "rb") as f:
        return f.read()


def flags_text(f):
    names = ["wdt", "lockout", "core", "TEST", "updating", "INSTALLED", "REVERTED"]
    return ",".join(n for i, n in enumerate(names) if f & (1 << i)) or "-"


def show(link, what):
    s = link.status
    if s:
        log(f"{what}: {s['fw']}, image {s['image'] or '-'}, update {s['update'] or '-'}, "
            f"flags {flags_text(s['flags'])}, "
            f"uptime {s['uptime']} s")
    return s


def wait_restart(link, timeout=60):
    """The S32K restarts: seen as its uptime going back in the status (a start without a swap takes less than
    the 3 s after which the HELLO counts as gone). Returns (gap in s, status after) or (None, None)."""
    n = link.restarts
    end = time.time() + timeout
    while time.time() < end:
        if link.restarts > n:
            return link.restart_gap, link.status
        time.sleep(0.05)
    return None, None


def pause(link):
    a = link.cmd(bl.CMD_PAUSE, 1, 0)
    return a is not None and a[2] == 0 and a[3] == 0


def begin_when_paused(snd, link, timeout=15):
    end = time.time() + timeout
    while True:
        status, _ = snd.begin()
        if status != -205 or time.time() > end:
            return status
        time.sleep(1.0)


def transfer(link, image, label, corrupt_at=None, probe=False):
    """BEGIN (after a pause), all DATA, FINISH. Returns the FINISH status, or the first error."""
    pause(link)
    snd = bl.OtaSender(link, image)
    status = begin_when_paused(snd, link)
    if status != 0:
        log(f"{label}: BEGIN {bl.err_name(status)}")
        return status
    t = time.time()
    if probe:
        # A lost ack: the same DATA again is taken as written. A wrong offset is refused with the right one.
        st, w = snd._xfer(bl.OTA_DATA, 0, image[0:1000])
        st2, w2 = snd._xfer(bl.OTA_DATA, 0, image[0:1000])
        check(st == 0 and st2 == 0 and w2 == 1000, f"{label}: DATA repeated after a lost ack taken once (offset {w2})")
        st3, w3 = snd._xfer(bl.OTA_DATA, 5000, image[5000:6000])
        check(st3 == -208 and w3 == 1000, f"{label}: DATA at a wrong offset refused, next offset {w3}")
        rest = bl.OtaSender(link, image, snd.image_id)
        rest.fid = snd.fid
        snd = rest
        snd_off = 1000
        while snd_off < len(image):
            st, w = snd._xfer(bl.OTA_DATA, snd_off, image[snd_off:snd_off + bl.OTA_DATA_MAX])
            if st != 0:
                log(f"{label}: DATA at {snd_off}: {bl.err_name(st)}")
                return st
            snd_off = w
        status = 0
    else:
        status, off = snd.data_all(corrupt_at=corrupt_at)
        if status != 0:
            log(f"{label}: DATA at {off}: {bl.err_name(status)}")
            return status
    dt = time.time() - t
    t = time.time()
    status, _ = snd.finish()
    log(f"{label}: {len(image)} bytes in {dt:.1f} s ({len(image) / dt / 1024:.1f} KB/s, {snd.repeats} repeats), "
        f"FINISH {bl.err_name(status)} after {time.time() - t:.1f} s")
    return status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("build")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--revert", action="store_true")
    a = ap.parse_args()

    good = sign(a.build, "0.6.1")
    other_board = sign(a.build, "0.6.2", board="some_other_board")
    log(f"images: {len(good)} bytes")

    link = bl.Link(a.port, a.baud)
    if not check(link.wait_peer(30), "HELLO from the S32K"):
        return 1
    st = link.wait_status(30)
    check(st is not None, "SNAP_S32K status")
    show(link, "before")
    before_image = st["image"] if st else ""

    # Refusals at BEGIN: nothing changes, the core keeps running
    esp = bytearray(good)
    esp[0] = 0xE9
    snd = bl.OtaSender(link, bytes(esp))
    check(snd.begin()[0] == -201, "an ESP32 file is refused (NOT_IMAGE)")
    snd = bl.OtaSender(link, good + bytes(400 * 1024))
    check(snd.begin()[0] == -202, "an image larger than slot 1 is refused (TOO_BIG)")
    if st and st["flags"] & bl.F_CORE:
        # The core runs (variant linksim): the safety refusals. Its simulated battery reports 5 A, so no
        # update can start on it; the update cycle runs on the variant "link" (core off).
        link.cmd(bl.CMD_PAUSE, 0, 0)  # running
        time.sleep(2.0)
        snd = bl.OtaSender(link, good)
        status = snd.begin()[0]
        check(status == -205, f"BEGIN while the emulator runs is refused (NOT_PAUSED): {bl.err_name(status)}")
        pause(link)
        time.sleep(2.0)
        status = snd.begin()[0]
        check(status == -206, f"paused, but 5 A from the simulated battery: refused (CURRENT): {bl.err_name(status)}")
        if status == 0:
            snd.abort()
            wait_restart(link)
        link.cmd(bl.CMD_PAUSE, 0, 0)
        st = link.wait_status(5, newer_than=time.time())
        check(st is not None and st["flags"] & bl.F_CORE and not st["flags"] & bl.F_UPDATING,
              "after the refusals the core still runs")
        return summary(link, before_image)

    # The update
    status = transfer(link, good, "update 0.6.1", probe=True)
    check(status == 0, "update 0.6.1 checked and marked")
    t = time.time()
    gap, st = wait_restart(link)
    check(st is not None, f"S32K back after the swap, status again {time.time() - t:.1f} s after FINISH "
          "(2 s delay, restart, swap, start)")
    show(link, "after the swap")
    check(st is not None and st["image"] == "0.6.1" and st["flags"] & bl.F_TEST,
          "running 0.6.1 as a test image")
    st = link.wait_status(120, lambda s: s["flags"] & bl.F_INSTALLED, newer_than=time.time())
    show(link, "after the confirm")
    check(st is not None and not st["flags"] & bl.F_TEST and st["update"] == "0.6.1",
          "0.6.1 confirmed itself (link up, 60 s) and reports installed")

    # Images that fail their check at FINISH: the S32K restarts into the running image
    status = transfer(link, good, "corrupted image", corrupt_at=200000)
    check(status == -210, f"a corrupted image is refused at FINISH (HASH): {bl.err_name(status)}")
    gap, st = wait_restart(link)
    check(st is not None and st["image"] == "0.6.1" and not st["flags"] & bl.F_TEST,
          "after the refused image: restart into 0.6.1, nothing marked")
    status = transfer(link, other_board, "image for another board")
    check(status == -212, f"an image for another board is refused at FINISH (BOARD): {bl.err_name(status)}")
    gap, st = wait_restart(link)
    check(st is not None and st["image"] == "0.6.1", "after the other board's image: restart into 0.6.1")

    # Abort and a stalled transfer
    pause(link)
    snd = bl.OtaSender(link, good)
    if begin_when_paused(snd, link) == 0:
        for off in range(0, 5000, 1000):
            snd._xfer(bl.OTA_DATA, off, good[off:off + 1000])
        check(snd.abort()[0] == 0, "ABORT acknowledged")
        gap, st = wait_restart(link)
        check(st is not None and st["image"] == "0.6.1" and not st["flags"] & bl.F_TEST,
              "after ABORT: restart into 0.6.1, nothing marked")
    pause(link)
    snd = bl.OtaSender(link, good)
    if begin_when_paused(snd, link) == 0:
        t = time.time()
        gap, st = wait_restart(link, timeout=30)
        check(st is not None and st["image"] == "0.6.1",
              f"a transfer that stops: restart into 0.6.1, {time.time() - t:.0f} s after BEGIN")

    if a.revert:
        image = sign(a.build, "0.6.3")
        status = transfer(link, image, "update 0.6.3 (revert test)")
        check(status == 0, "update 0.6.3 marked")
        gap, st = wait_restart(link)
        check(st is not None and st["image"] == "0.6.3" and st["flags"] & bl.F_TEST, "0.6.3 on test")
        log("HELLO stopped: 0.6.3 must not confirm and restart after 5 minutes")
        link.hello_on = False
        t = time.time()
        # HELLO from the S32K keeps coming while it runs; it stops at the restart
        end = time.time() + 420
        while time.time() < end and link.peer_alive():
            time.sleep(0.5)
        log(f"S32K restarted {time.time() - t:.0f} s after HELLO stopped")
        link.hello_on = True
        link.wait_peer(60)
        st = link.wait_status(60, newer_than=time.time())
        show(link, "after the revert")
        check(st is not None and st["image"] == "0.6.1" and st["flags"] & bl.F_REVERTED and st["update"] == "0.6.3",
              "0.6.3 reverted: 0.6.1 back, reports the update as reverted")

    return summary(link, before_image)


def summary(link, before_image):
    log(f"link frames {link.stats['frames']}, bad {link.stats['bad']}; image before {before_image or '-'}")
    link.close()
    failed = [w for ok, w in results if not ok]
    log(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
