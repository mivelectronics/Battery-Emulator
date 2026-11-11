"""A STAND-IN for the power-cut tests (POWER_CUT_TESTS.md): resets the S32K through the PEmicro OpenSDA debugger at
a chosen point of an update sent through the ESP32, as the /miv/update page sends it. The power stays on: see
POWER_CUT_TESTS.md for what this does not cover.

    python ota_reset_test.py <esp32 ip> <app_V.bin> <percent> [hold seconds]

At <percent> of the bytes sent the PEmicro debugger server starts: it halts the core as it connects to the chip (the
stand-in cut, a few seconds later than <percent>); after [hold seconds] more (default 0, as a power cut
lasts) gdb resets the core into a run and detaches. The sending goes on
as a browser would, until the ESP32 gives up. Run with the Python of zw/.venv, from Git Bash or PowerShell.
"""
import json
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

PE = r"C:\PEMicro\gdbserver"
SERVER = PE + r"\pegdbserver_console.exe"
GDB = PE + r"\gdb\arm-none-eabi-gdb.exe"
DEVICE = "NXP_S32K3xx_S32K312"
PORT = 7224
PIECE = 4096

ip, path, percent = sys.argv[1], sys.argv[2], float(sys.argv[3])
hold = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
image = open(path, "rb").read()
t0 = time.time()


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {time.time() - t0:7.1f} {msg}", flush=True)


def call(method, p, body=None):
    req = urllib.request.Request(f"http://{ip}{p}", data=body, method=method,
                                 headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        text = e.read()
        try:
            return e.code, json.loads(text)
        except ValueError:
            return e.code, {"text": text.decode(errors="replace")}


def status():
    return call("GET", "/miv/api/status")[1]


hw = subprocess.run([SERVER, "-showhardware"], capture_output=True, text=True).stdout
hwport = next((w.split("=")[1] for w in hw.split() if w.lstrip("-").startswith("PORT=PEM")), None)
if not hwport:
    log("no PEmicro interface found")
    sys.exit(2)
srv = None
srv_log = None
reset_at = {}


def written():
    return call("GET", "/miv/api/s32k/ota/state")[1].get("written")


def reset():
    """The debugger server halts the core as it connects to the chip: that is the moment of the stand-in cut.
    gdb then resets the core into a run and detaches."""
    global srv, srv_log
    reset_at["t"] = time.time() - t0
    log(f"CUT: debugger server starts (halts the S32K), {written()} bytes written on the S32K")
    srv_log = open("ota_reset_pe.log", "w")
    srv = subprocess.Popen([SERVER, "-startserver", f"-device={DEVICE}", "-interface=USBMULTILINK",
                            f"-port={hwport}", f"-serverport={PORT}", "-singlesession"],
                           stdout=srv_log, stderr=subprocess.STDOUT)
    for _ in range(60):
        time.sleep(0.25)
        if "All Servers Running" in open("ota_reset_pe.log", encoding="utf-8", errors="replace").read():
            break
    log(f"debugger server running, S32K halted; {written()} bytes written on the S32K")
    if hold:
        time.sleep(hold)
        log(f"held {hold:.0f} s more; {written()} bytes written on the S32K")
    subprocess.run([GDB, "-q", "-batch", "-ex", "set pagination off", "-ex", f"target remote localhost:{PORT}",
                    "-ex", "monitor _reset", "-ex", "detach"], capture_output=True, text=True)
    log(f"S32K reset and running ({time.time() - t0 - reset_at['t']:.1f} s after the cut started)")


try:
    code, s = call("POST", f"/miv/api/s32k/ota/begin?size={len(image)}", image[:32])
    if code != 200:
        log(f"refused: {s.get('text')} ({s.get('error')})")
        sys.exit(1)
    log(f"update {s.get('version')}, {len(image)} bytes, reset at {percent:.0f} %")
    while True:
        code, s = call("GET", "/miv/api/s32k/ota/state")
        if s.get("phase") != "starting":
            break
        time.sleep(0.1)
    if s.get("phase") != "sending":
        log(f"refused by the S32K: {s.get('text')} ({s.get('error')})")
        sys.exit(1)
    off = 0
    th = None
    fail = None
    while off < len(image):
        if th is None and off >= len(image) * percent / 100:
            log(f"{off} bytes sent ({100 * off / len(image):.1f} %), {s.get('written')} written on the S32K")
            th = threading.Thread(target=reset)
            th.start()
        code, s = call("POST", f"/miv/api/s32k/ota/chunk?offset={off}", image[off:off + PIECE])
        if code == 503:
            if th is not None and time.time() - t0 - reset_at.get("t", 0) > 60:
                fail = "the ESP32 still busy 60 s after the cut"
                break
            time.sleep(0.05)
            continue
        if code != 200:
            fail = f"chunk at {off}: HTTP {code}: {s.get('text')} ({s.get('error')})"
            break
        off = s["received"]
    log(fail or f"all {off} bytes sent")
    if not fail:
        call("POST", "/miv/api/s32k/ota/finish")
    for _ in range(150):
        code, s = call("GET", "/miv/api/s32k/ota/state")
        if s.get("phase") not in ("sending", "checking", "starting"):
            break
        time.sleep(0.2)
    log(f"ESP32 transfer: {s.get('phase')} {s.get('version')} {s.get('written')}/{s.get('total')} "
        f"error {s.get('error')}: {s.get('text')}")
    if th:
        th.join()
finally:
    if srv:
        srv.kill()
        srv.wait()
        srv_log.close()
