"""One timeline for the power-cut tests (OTA_PLAN.md step 4): the S32K console on the OpenSDA port (MCUboot and the
application in the prodslow / prod variants), reopened after the port goes away with the power, and the state the
ESP32 reports (S32K image, test image, last update, the transfer), logged when it changes.

    python power_test_log.py <S32K COM> <esp32 ip> <seconds> [log file]

Run with the Python of zw/.venv. Stops after <seconds> or Ctrl+C.
"""
import json
import sys
import threading
import time
import urllib.request

import serial

port, ip, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
out = open(sys.argv[4], "a", encoding="utf-8") if len(sys.argv) > 4 else None
t0 = time.time()
lock = threading.Lock()


def log(src, msg):
    line = f"{time.strftime('%H:%M:%S')} {time.time() - t0:7.1f} {src} {msg}"
    with lock:
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()


def console():
    s = None
    gone = False
    buf = b""
    while time.time() - t0 < secs:
        if s is None:
            try:
                s = serial.Serial(port, 115200, timeout=0.2)
                if gone:
                    log("COM", f"--- {port} back")
                gone = False
            except Exception:
                if not gone:
                    log("COM", f"--- {port} gone (power off?)")
                gone = True
                time.sleep(0.5)
                continue
        try:
            d = s.read(4096)
        except Exception:
            s = None
            log("COM", f"--- {port} gone (power off?)")
            gone = True
            continue
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            if text and not text.startswith("cfg rev"):
                log("S32K", text)


def esp():
    keys = ("s32k_image", "s32k_test_image", "s32k_update", "s32k_update_result", "src")
    last = None
    last_up = None
    last_ota = None
    down = False
    while time.time() - t0 < secs:
        try:
            with urllib.request.urlopen(f"http://{ip}/miv/api/status", timeout=3) as r:
                st = json.loads(r.read())
            with urllib.request.urlopen(f"http://{ip}/miv/api/s32k/ota/state", timeout=3) as r:
                ota = json.loads(r.read())
            if down:
                log("ESP", "--- ESP32 answers again")
            down = False
        except Exception:
            if not down:
                log("ESP", "--- ESP32 does not answer (power off?)")
            down = True
            time.sleep(1)
            continue
        now = {k: st.get(k) for k in keys}
        if now != last:
            log("ESP", "status " + " ".join(f"{k}={v}" for k, v in now.items()))
            last = now
        up = st.get("s32k_uptime_s")
        if up is not None and last_up is not None and up + 3 < last_up:
            log("ESP", f"S32K restarted (uptime {last_up} -> {up} s)")
        last_up = up
        o = (ota.get("phase"), ota.get("text"))
        if o != last_ota:
            log("ESP", f"transfer {ota.get('phase')} {ota.get('version')} {ota.get('written')}/{ota.get('total')} "
                       f"{ota.get('text') or ''}")
            last_ota = o
        time.sleep(1)


threading.Thread(target=esp, daemon=True).start()
try:
    console()
except KeyboardInterrupt:
    pass
log("LOG", "end")
