"""STAND-INS for the power-cut tests 2-6 of POWER_CUT_TESTS.md, run without anyone at the boards, each cut set off by
an event (the transfer progress, a line of the S32K console), not by hand. The power is NOT cut:
- S32K: the PEmicro debugger server halts the core as it connects to the chip; after a hold, gdb resets it into a run;
- ESP32: its EN held low through RTS of its USB serial port (the auto-reset circuit, IO0 left high), then released.
POWER_CUT_TESTS.md says what these do not cover.

    python ota_cut_tests.py <S32K COM> <ESP32 COM> <esp32 ip> <images dir> <log> [test ...]

Tests: 2 3 4 5 6 final (default: all, in that order). Images: <images dir>/s32k_app_0.7.<n>.bin as in the plan.
Stops at the first test that does not pass. Run with the Python of zw/.venv; the prodslow build on the S32K.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

import serial

PE = r"C:\PEMicro\gdbserver"
SERVER = PE + r"\pegdbserver_console.exe"
GDB = PE + r"\gdb\arm-none-eabi-gdb.exe"
DEVICE = "NXP_S32K3xx_S32K312"
GDB_PORT = 7224
PIECE = 4096

s32k_com, esp_com, ip, img_dir, log_path = sys.argv[1:6]
tests = sys.argv[6:] or ["2", "3", "4", "5", "6", "final"]
out = open(log_path, "a", encoding="utf-8")
t0 = time.time()
lock = threading.Lock()
console = []  # (time, line) of the S32K console
stop = False


def log(src, msg):
    line = f"{time.strftime('%H:%M:%S')} {time.time() - t0:7.1f} {src} {msg}"
    with lock:
        print(line, flush=True)
        out.write(line + "\n")
        out.flush()


class Failed(Exception):
    pass


def check(ok, what):
    log("CHECK", ("ok    " if ok else "FAILED ") + what)
    if not ok:
        raise Failed(what)


# --- the S32K console -----------------------------------------------------------------------------------------------
def console_reader():
    s = None
    buf = b""
    while not stop:
        if s is None:
            try:
                s = serial.Serial(s32k_com, 115200, timeout=0.2)
            except Exception:
                time.sleep(0.5)
                continue
        try:
            d = s.read(4096)
        except Exception:
            s = None
            log("COM", f"--- {s32k_com} gone")
            continue
        buf += d
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").rstrip()
            if text and not text.startswith("cfg rev") and not re.match(r"CAN\d|modbus|\*\*\* (Using|Booting) Z", text):
                with lock:
                    console.append((time.time(), text))
                log("S32K", text)


def mark():
    with lock:
        return len(console)


def wait_line(pattern, since, timeout):
    """The first console line from index since matching pattern, or None after timeout."""
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for t, text in console[since:]:
                if re.search(pattern, text):
                    return text
        time.sleep(0.05)
    return None


def lines_since(since):
    with lock:
        return [text for _, text in console[since:]]


# --- the ESP32 over http --------------------------------------------------------------------------------------------
def call(method, p, body=None, timeout=15):
    req = urllib.request.Request(f"http://{ip}{p}", data=body, method=method,
                                 headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        text = e.read()
        try:
            return e.code, json.loads(text)
        except ValueError:
            return e.code, {"text": text.decode(errors="replace")}


def get(p):
    try:
        return call("GET", p, timeout=3)[1]
    except Exception:
        return None


def esp_poller():
    keys = ("s32k_image", "s32k_test_image", "s32k_update", "s32k_update_result")
    last = last_ota = None
    last_up = None
    down = False
    while not stop:
        st = get("/miv/api/status")
        ota = get("/miv/api/s32k/ota/state") if st else None
        if st is None or ota is None:
            if not down:
                log("ESP", "--- ESP32 does not answer")
            down = True
            time.sleep(0.5)
            continue
        if down:
            log("ESP", f"--- ESP32 answers again (uptime {st.get('esp_uptime_s')} s)")
        down = False
        now = {k: st.get(k) for k in keys}
        if now != last:
            log("ESP", "status " + " ".join(f"{k}={v}" for k, v in now.items()))
            last = now
        up = st.get("s32k_uptime_s")
        if up is not None and last_up is not None and up + 3 < last_up:
            log("ESP", f"S32K restarted (uptime {last_up} -> {up} s)")
        last_up = up
        o = (ota.get("phase"), ota.get("text"), ota.get("version"))
        if o != last_ota:
            log("ESP", f"transfer {ota.get('phase')} {ota.get('version')} {ota.get('written')}/{ota.get('total')} "
                       f"{ota.get('text') or ''}")
            last_ota = o
        time.sleep(0.5)


def wait_status(pred, timeout, what):
    end = time.time() + timeout
    st = None
    while time.time() < end:
        st = get("/miv/api/status")
        if st and st.get("s32k_image") and pred(st):
            return st
        time.sleep(1)
    check(False, f"{what} within {timeout} s (last: { {k: (st or {}).get(k) for k in ('s32k_image', 's32k_test_image', 's32k_update', 's32k_update_result')} })")


def store_check(want_current, not_versions):
    # the ESP32 marks its copy a moment after it reports the result: up to 10 s
    for _ in range(20):
        s = get("/miv/api/s32k/store")
        slots = (s or {}).get("slots", [])
        if any(x["state"] == "current" and x["version"] == want_current for x in slots):
            break
        time.sleep(0.5)
    log("ESP", "store " + "; ".join(f"slot {x['slot']}: {x['state']} {x['version']}" for x in slots))
    check(any(x["state"] == "current" and x["version"] == want_current for x in slots),
          f"ESP32 copy of {want_current} is current")
    for v in not_versions:
        check(not any(x["version"] == v for x in slots), f"no ESP32 copy of {v} kept")


# --- the stand-in cuts ----------------------------------------------------------------------------------------------
hw = subprocess.run([SERVER, "-showhardware"], capture_output=True, text=True).stdout
HWPORT = next((w.split("=")[1] for w in hw.split() if w.lstrip("-").startswith("PORT=PEM")), None)


def s32k_cut(hold):
    """Halt (the debugger server connecting), hold, reset into a run."""
    log("CUT", f"S32K: debugger server starts (halts the core), hold {hold} s")
    pe_log = os.path.join(os.path.dirname(log_path), "ota_cut_pe.log")
    with open(pe_log, "w") as f:
        srv = subprocess.Popen([SERVER, "-startserver", f"-device={DEVICE}", "-interface=USBMULTILINK",
                                f"-port={HWPORT}", f"-serverport={GDB_PORT}", "-singlesession"],
                               stdout=f, stderr=subprocess.STDOUT)
        for _ in range(80):
            time.sleep(0.25)
            if "All Servers Running" in open(pe_log, encoding="utf-8", errors="replace").read():
                break
        log("CUT", "S32K halted (debugger server running)")
        time.sleep(hold)
        r = subprocess.run([GDB, "-q", "-batch", "-ex", "set pagination off", "-ex",
                            f"target remote localhost:{GDB_PORT}", "-ex", "monitor _reset", "-ex", "detach"],
                           capture_output=True, text=True)
        srv.kill()
        srv.wait()
    ok = "_reset" in r.stdout + r.stderr
    log("CUT", f"S32K reset into a run ({'ok' if ok else 'gdb: ' + (r.stdout + r.stderr)[-200:]})")


def esp_cut(hold):
    """EN low through RTS (DTR off: IO0 high, a normal start), hold, release."""
    log("CUT", f"ESP32: EN low through RTS of {esp_com}, hold {hold} s")
    p = serial.Serial()
    p.port = esp_com
    p.baudrate = 115200
    p.dtr = False
    p.rts = False
    p.open()
    p.rts = True
    time.sleep(hold)
    p.rts = False
    p.close()
    log("CUT", "ESP32: EN released")


# --- an update as the page sends it, with a cut at a point of it ------------------------------------------------
def image(n):
    return open(os.path.join(img_dir, f"s32k_app_0.7.{n}.bin"), "rb").read()


def send(img, cut_at=None, cut=None):
    """Sends img; at cut_at (fraction of the bytes) runs cut() in a thread and goes on as a browser would until the
    ESP32 refuses or goes away. Returns (final transfer state or None if the ESP32 went away, cut thread)."""
    code, s = call("POST", f"/miv/api/s32k/ota/begin?size={len(img)}", img[:32])
    check(code == 200, f"ESP32 takes the update ({s.get('version')}): {code} {s.get('text') or ''}")
    while True:
        code, s = call("GET", "/miv/api/s32k/ota/state")
        if s.get("phase") != "starting":
            break
        time.sleep(0.1)
    check(s.get("phase") == "sending", f"S32K takes BEGIN ({s.get('text') or s.get('phase')})")
    off = 0
    th = None
    t_cut = None
    while off < len(img):
        if cut and th is None and off >= len(img) * cut_at:
            log("SEND", f"{off} bytes sent ({100 * off / len(img):.0f} %), {s.get('written')} written on the S32K")
            th = threading.Thread(target=cut)
            th.start()
            t_cut = time.time()
        try:
            code, s = call("POST", f"/miv/api/s32k/ota/chunk?offset={off}", img[off:off + PIECE], timeout=10)
        except Exception as e:
            log("SEND", f"the ESP32 went away at {off}: {type(e).__name__}")
            return None, th
        if code == 503:
            check(t_cut is None or time.time() - t_cut < 60, "the ESP32 not still busy 60 s after the cut")
            time.sleep(0.05)
            continue
        if code != 200:
            log("SEND", f"piece at {off}: HTTP {code}: {s.get('text')} ({s.get('error')})")
            break
        off = s["received"]
    if off >= len(img):
        call("POST", "/miv/api/s32k/ota/finish")
    for _ in range(300):
        s = get("/miv/api/s32k/ota/state")
        if s and s.get("phase") not in ("sending", "checking", "starting"):
            break
        time.sleep(0.2)
    s = s or {}
    log("SEND", f"transfer: {s.get('phase')} {s.get('version')} {s.get('written')}/{s.get('total')} "
                f"error {s.get('error')}: {s.get('text')}")
    return s, th


def boot_lines(since, timeout=120):
    """Waits for the application start after since; returns the console lines from since to it."""
    line = wait_line(r"^image \S+: ", since, timeout)
    check(line is not None, f"the application starts within {timeout} s")
    time.sleep(0.5)
    return lines_since(since)


def seen(lines, pattern):
    return any(re.search(pattern, x) for x in lines)


def before(lines, first, second):
    """first appears and comes before the first second"""
    i = next((k for k, x in enumerate(lines) if re.search(first, x)), None)
    j = next((k for k, x in enumerate(lines) if re.search(second, x)), len(lines))
    return i is not None and i < j


# --- the tests --------------------------------------------------------------------------------------------------
def test2():
    """ESP32 reset during the transfer (0.7.1): the S32K restarts into its image after 10 s without data."""
    m = mark()
    s, th = send(image(1), 0.45, lambda: esp_cut(3))
    th.join()
    line = wait_line(r"update: .*restarting into the running image", m, 30)
    check(line is not None, f"S32K gives the update up by itself: {line}")
    lines = boot_lines(m)
    check(seen(lines, r"Swap type: none"), "MCUboot: swap type none")
    check(seen(lines, r"^image 0\.7\.0: confirmed"), "S32K runs 0.7.0, confirmed")
    st = wait_status(lambda x: x.get("s32k_image") == "0.7.0", 60, "the ESP32 back and sees 0.7.0")
    check(not st.get("s32k_test_image"), "no test image")
    ota = get("/miv/api/s32k/ota/state")
    log("ESP", f"transfer state after its restart: {ota}")
    store_check("0.7.0", ["0.7.1"])


def full_update(n, cut_line=None, cut_wait=0, hold=3):
    """Sends 0.7.n all the way; if cut_line, cuts the S32K cut_wait s after that console line."""
    m = mark()
    s, _ = send(image(n))
    check(s and s.get("phase") == "done", f"transfer of 0.7.{n} done")
    if cut_line:
        line = wait_line(cut_line, m, 120)
        check(line is not None, f"console: {line}")
        time.sleep(cut_wait)
        m2 = mark()
        s32k_cut(hold)
        return m2
    return m


def test3():
    """S32K cut during the (slowed) swap of 0.7.2: MCUboot resumes it, 0.7.2 on test, confirms."""
    m = full_update(2, r"swap starts, slowed down", 15)
    lines = boot_lines(m)
    check(not seen(lines, r"swap starts, slowed down"), "the resumed swap is not reported as a new one (not slowed)")
    check(seen(lines, r"Image version: v0\.7\.2"), "MCUboot starts 0.7.2")
    check(seen(lines, r"^image 0\.7\.2: test image, not confirmed"), "0.7.2 runs on test")
    line = wait_line(r"^image confirmed: 0", m, 90)
    check(line is not None, "0.7.2 confirms itself")
    st = wait_status(lambda x: x.get("s32k_update_result") == "installed", 30, "the page: installed")
    check(st.get("s32k_image") == "0.7.2", "the page: 0.7.2")
    store_check("0.7.2", [])


def test4():
    """S32K cut while 0.7.3 runs on test: MCUboot swaps 0.7.2 back (slowed)."""
    m = full_update(3, r"^image 0\.7\.3: test image, not confirmed", 20)
    line = wait_line(r"swap starts, slowed down", m, 30)
    check(line is not None, "MCUboot swaps back (slowed, red LED)")
    lines = boot_lines(m, 120)
    check(seen(lines, r"Image version: v0\.7\.2"), "MCUboot starts 0.7.2")
    check(seen(lines, r"^image 0\.7\.2: confirmed"), "0.7.2 confirmed")
    check(seen(lines, r"update to 0\.7\.3 did not confirm"), "S32K: update to 0.7.3 did not confirm")
    st = wait_status(lambda x: x.get("s32k_update_result") == "reverted", 30, "the page: reverted")
    check(st.get("s32k_update") == "0.7.3" and st.get("s32k_image") == "0.7.2", "the page: 0.7.3 reverted, 0.7.2 runs")
    store_check("0.7.2", [])


def test5():
    """S32K cut while 0.7.4 runs on test, then again during the swap back: MCUboot resumes the swap back."""
    m = full_update(4, r"^image 0\.7\.4: test image, not confirmed", 20)
    line = wait_line(r"swap starts, slowed down", m, 30)
    check(line is not None, "MCUboot swaps back (slowed, red LED)")
    time.sleep(15)
    m2 = mark()
    s32k_cut(3)
    lines = boot_lines(m2, 120)
    check(not seen(lines, r"swap starts, slowed down"), "the resumed swap back is not reported as a new one")
    check(seen(lines, r"Image version: v0\.7\.2"), "MCUboot starts 0.7.2")
    check(seen(lines, r"^image 0\.7\.2: confirmed"), "0.7.2 confirmed")
    check(seen(lines, r"update to 0\.7\.4 did not confirm"), "S32K: update to 0.7.4 did not confirm")
    st = wait_status(lambda x: x.get("s32k_update_result") == "reverted", 30, "the page: reverted")
    check(st.get("s32k_update") == "0.7.4" and st.get("s32k_image") == "0.7.2", "the page: 0.7.4 reverted, 0.7.2 runs")
    store_check("0.7.2", [])


def test6():
    """Both cut during the transfer of 0.7.5."""
    def both():
        t = threading.Thread(target=lambda: esp_cut(5))
        t.start()
        s32k_cut(5)
        t.join()
    m = mark()
    s, th = send(image(5), 0.45, both)
    th.join()
    lines = boot_lines(m, 60)
    check(seen(lines, r"Swap type: none"), "MCUboot: swap type none")
    check(seen(lines, r"^image 0\.7\.2: confirmed"), "S32K runs 0.7.2, confirmed")
    st = wait_status(lambda x: x.get("s32k_image") == "0.7.2", 60, "the ESP32 back and sees 0.7.2")
    check(not st.get("s32k_test_image"), "no test image")
    store_check("0.7.2", ["0.7.5"])


def final():
    """One update all the way: 0.7.5."""
    m = full_update(5)
    line = wait_line(r"^image 0\.7\.5: test image, not confirmed", m, 120)
    check(line is not None, "0.7.5 runs on test")
    check(wait_line(r"^image confirmed: 0", m, 90) is not None, "0.7.5 confirms itself")
    st = wait_status(lambda x: x.get("s32k_update_result") == "installed", 30, "the page: installed")
    check(st.get("s32k_image") == "0.7.5", "the page: 0.7.5")
    store_check("0.7.5", [])


threading.Thread(target=console_reader, daemon=True).start()
threading.Thread(target=esp_poller, daemon=True).start()
time.sleep(2)
result = 0
for name in tests:
    fn = globals()["test" + name] if name != "final" else final
    log("TEST", f"=== test {name}: {fn.__doc__}")
    try:
        fn()
        log("TEST", f"=== test {name}: PASSED")
    except Failed as e:
        log("TEST", f"=== test {name}: FAILED ({e}); the rest not run")
        result = 1
        break
time.sleep(2)
stop = True
log("LOG", "end")
sys.exit(result)
