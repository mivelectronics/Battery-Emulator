"""Test of the contactor outputs and the watchdog, with the bench simulator and without any wire.

Build as for net_snapshot_test.py. Usage: python contactor_test.py COM13 <ESP32 IP> [resets]

Output levels are read back from the pads and their changes come from a trace in the firmware, so this
shows what the pins do and when, but nothing about the driver or coil behind them. Hangs and faults are
caused on request by the simulator (register 400). Leaves the S32K settings at their defaults.
"""
import re
import sys
import time
import urllib.parse
import urllib.request

from pymodbus.client import ModbusSerialClient

port, ip = sys.argv[1:3]
RESETS = int(sys.argv[3]) if len(sys.argv) > 3 else 30
FLASH_WRITES = 400
client = ModbusSerialClient(port=port, baudrate=19200, parity="E", stopbits=1, bytesize=8, timeout=1, retries=0)
assert client.connect(), "no serial port"
failures = 0

POS, NEG, PRE, BAT2, BMS = 0, 1, 2, 3, 4
NAMES = {POS: "positive", NEG: "negative", PRE: "precharge", BAT2: "battery 2", BMS: "BMS power"}

REG_CAUSE, REG_IN_ROW, REG_OUT_AFTER_RESET, REG_WDT_RUNNING = 10, 11, 12, 13
REG_ENGAGED, REG_LEVELS, REG_LOCKED_OUT = 205, 226, 227
REG_CHG_MAX = 213
RST_REQUESTED, RST_WDT_FIRST, RST_FATAL, RST_WDT_HW = 1, 2, 3, 4
CMD_SILENT, CMD_BACK = 1, 2
HANG_SPIN, HANG_NO_IRQ, HANG_SLEEP, HANG_BUSFAULT, HANG_MAIN = 11, 12, 13, 14, 15


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail != "" else ""), flush=True)
    failures += 0 if ok else 1


def rd(addr, n=1, tries=8):
    for _ in range(tries):
        try:
            r = client.read_holding_registers(addr, count=n, device_id=1)
            if not r.isError():
                return r.registers
        except Exception:
            pass
        time.sleep(0.3)
    raise RuntimeError(f"read {addr} failed")


def wr(addr, value):
    for _ in range(4):
        try:
            r = client.write_register(addr, value, device_id=1)
            if not r.isError():
                return
        except Exception:
            pass
        time.sleep(0.3)
    raise RuntimeError(f"write {addr} failed")


def uptime():
    hi, lo = rd(2, 2)
    return (hi << 16) | lo


def wait_for(cond, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond():
                return time.time() - t0
        except Exception:
            pass
        time.sleep(0.2)
    return None


def hang_now(cmd):
    """Once only: an answer may not come back, and a repeat would reach the restarted firmware"""
    try:
        client.write_register(400, cmd, device_id=1)
    except Exception:
        pass


def wait_restart(before, secs=20):
    """Waits for the uptime to fall below what it was just read to be"""
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if uptime() < before:
                return time.time() - t0
        except Exception:
            pass
        time.sleep(0.1)
    return None


def reboot():
    # Right after a restart the uptime is too small to see it fall
    wait_for(lambda: uptime() >= 3, 10)
    before = uptime()
    wr(5000, 0xA5C3)
    wr(5001, 0xB007)
    assert wait_restart(before) is not None, "no restart"


def settings(**regs):
    """Writes settings registers (r5054=1) and saves"""
    wr(5000, 0xA5C3)
    for k, v in regs.items():
        wr(int(k[1:]), v)
    wr(5001, 1)
    result = rd(5001)[0]
    wr(5000, 0)
    return result


def trace():
    out = []
    n = rd(420)[0]
    for i in range(n):
        p, hi, lo = rd(421 + 3 * i, 3)
        out.append((p & 0xFF, p >> 8, (hi << 16) | lo))
    return out


def levels():
    return rd(REG_LEVELS)[0]


def closed():
    return (levels() & 3) == 3 and rd(REG_ENGAGED)[0] == 1


def web(path, data=None):
    return urllib.request.urlopen(urllib.request.Request(f"http://{ip}{path}", data=data), timeout=15).read().decode(
        "utf-8", "replace")


def page_text(path):
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", web(path)))


def settings_form():
    s = web("/settings")
    i = s.find("action='saveSettings'")
    form = s[i:s.find("</form>", i)]
    data = {}
    for m in re.finditer(r"<input\b[^>]*>", form, re.I):
        t = m.group(0)
        n = re.search(r"name=['\"]([^'\"]+)", t)
        if not n:
            continue
        v = re.search(r"value=['\"]([^'\"]*)", t)
        ty = re.search(r"type=['\"]([^'\"]+)", t)
        ty = ty.group(1).lower() if ty else "text"
        if ty == "checkbox":
            if re.search(r"\bchecked\b", t):
                data[n.group(1)] = v.group(1) if v else "on"
        elif ty != "submit":
            data[n.group(1)] = v.group(1) if v else ""
    for m in re.finditer(r"<select\b[^>]*name=['\"]([^'\"]+)[^>]*>(.*?)</select>", form, re.I | re.S):
        o = re.search(r"<option[^>]*value=['\"]([^'\"]*)['\"][^>]*selected", m.group(2), re.I)
        if o:
            data[m.group(1)] = o.group(1)
    return data


def save_form(data):
    web("/saveSettings", urllib.parse.urlencode(data).encode())


print("---- the watchdog runs, nothing is driven without the setting", flush=True)
wait_for(lambda: uptime() >= 0, 10)
check("settings at the defaults", settings(r5054=0, r5055=0, r5056=0, r5057=100, r5041=0) == 0)
reboot()
check("watchdog running", rd(REG_WDT_RUNNING)[0] == 1)
check("reset cause: requested", rd(REG_CAUSE)[0] == RST_REQUESTED, rd(REG_CAUSE)[0])
check("no output left driven by the reset", rd(REG_OUT_AFTER_RESET)[0] == 0, rd(REG_OUT_AFTER_RESET)[0])
time.sleep(13)
check("contactor control off: no output moves", levels() == 0 and trace() == [], (levels(), trace()))

print("---- closing sequence", flush=True)
check("contactor control on, precharge 300 ms", settings(r5054=1, r5057=300) == 0)
check("too short a precharge is refused", settings(r5057=2) != 0)
reboot()
time.sleep(5)
check("outputs still open 5 s after the start", levels() == 0, levels())
check("closes", wait_for(closed, 20) is not None, levels())
t = trace()
print("     " + ", ".join(f"{NAMES[p]}={lv} at {ms} ms" for p, lv, ms in t))
order = [(p, lv) for p, lv, _ in t]
check("order: negative, precharge, positive, precharge off", order == [(NEG, 1), (PRE, 1), (POS, 1), (PRE, 0)], order)
if len(t) == 4:
    d1, d2, d3 = t[1][2] - t[0][2], t[2][2] - t[1][2], t[3][2] - t[2][2]
    check("not before 10 s after the start", t[0][2] >= 10000, t[0][2])
    check("negative to precharge 500 ms", 490 <= d1 <= 530, d1)
    check("precharge to positive 300 ms", 290 <= d2 <= 330, d2)
    check("positive to precharge off 1000 ms", 990 <= d3 <= 1030, d3)
check("pads: positive and negative high, precharge low", levels() & 7 == 3, levels())

print("---- a change while closed waits for the restart", flush=True)
check("contactor control off in the settings", settings(r5054=0) == 0)
time.sleep(2)
check("the contactors stay under control", closed(), levels())
check("contactor control back on", settings(r5054=1) == 0)

print("---- second battery", flush=True)
check("second battery with its contactor", settings(r5041=1, r5055=1) == 0)
reboot()
check("closes", wait_for(closed, 25) is not None, levels())
check("battery 2 joins after the main pair", wait_for(lambda: levels() & (1 << BAT2), 40) is not None, levels())
t = trace()
print("     " + ", ".join(f"{NAMES[p]}={lv} at {ms} ms" for p, lv, ms in t))
check("after precharge off", [p for p, _, _ in t].index(BAT2) > [i for i, x in enumerate(t) if x[0] == PRE and x[1] == 0][0] - 1)
check("page shows the state", wait_for(lambda: "Contactors control — state: ON" in page_text("/"), 15) is not None)
check("page shows the second contactor", "Contactor for 2ⁿᵈ — state: ON" in page_text("/"))

print("---- equipment stop from the page", flush=True)
web("/equipmentStop?value=1")
check("opens all of them", wait_for(lambda: levels() & 0xF == 0, 15) is not None, levels())
check("engaged 0", rd(REG_ENGAGED)[0] == 0, rd(REG_ENGAGED)[0])
web("/equipmentStop?value=0")
check("closes again", wait_for(closed, 40) is not None, levels())
check("second battery too", wait_for(lambda: levels() & (1 << BAT2), 40) is not None, levels())
check("single battery again", settings(r5041=0, r5055=0) == 0)
reboot()
check("closes", wait_for(closed, 25) is not None)

print("---- settings from the page", flush=True)
form = settings_form()
check("page has the setting from the S32K", form.get("CNTCTRL") == "on", form.get("CNTCTRL"))
form["PWMCNTCTRL"] = "on"
form["PRECHGMS"] = "400"
save_form(form)
check("precharge time reaches the S32K", wait_for(lambda: rd(5057)[0] == 400, 10) is not None, rd(5057)[0])
check("PWM hold refused, page back to off", wait_for(lambda: "PWMCNTCTRL" not in settings_form(), 15) is not None)
form = settings_form()
form.pop("CNTCTRL", None)
save_form(form)
check("contactor control off from the page", wait_for(lambda: rd(5054)[0] == 0, 10) is not None, rd(5054)[0])
time.sleep(1)
check("the contactors stay under control until the restart", closed(), levels())
form = settings_form()
form["CNTCTRL"] = "on"
save_form(form)
check("and back on", wait_for(lambda: rd(5054)[0] == 1, 10) is not None, rd(5054)[0])

print("---- flash erases while the watchdog runs", flush=True)
# Enough writes to fill 8 KB NVS sectors several times, with the garbage collection that brings.
# The core thread asks for the stop state to be stored, the main thread does the flash work.
up0 = uptime()
for n in range(FLASH_WRITES):
    wr(5000, 0xA5C3)
    wr(5057, 100 + n % 2)
    wr(5001, 1)
check("settings saved over Modbus, no restart", uptime() > up0 and rd(REG_CAUSE)[0] == RST_REQUESTED, uptime())
print(f"     longest core tick so far: {rd(222)[0]} us", flush=True)
for n in range(FLASH_WRITES):
    web(f"/equipmentStop?value={(n + 1) % 2}")
    time.sleep(0.05)
web("/equipmentStop?value=0")
tick = rd(222)[0]
print(f"     longest core tick with the stop state stored: {tick} us", flush=True)
check("stop state saved, no restart", uptime() > up0 and rd(REG_CAUSE)[0] == RST_REQUESTED, uptime())
check("flash writes do not stall the core beyond 10 ms", tick < 10000, tick)
check("settings back", settings(r5057=300) == 0)
check("closes again", wait_for(closed, 40) is not None, levels())

print("---- stop button input, no wire on it", flush=True)
form = settings_form()
if "EQSTOP" in form:
    form["EQSTOP"] = "1"
    save_form(form)
    time.sleep(2)
    reboot()
    time.sleep(13)
    check("open wire reads as pressed: stays open", levels() & 7 == 0 and "EQUIPMENT_STOP" in web("/events"), levels())
    form = settings_form()
    form["EQSTOP"] = "0"
    save_form(form)
    time.sleep(2)
    web("/equipmentStop?value=0")
    time.sleep(1)
    reboot()
    check("closes without the setting", wait_for(closed, 40) is not None, levels())
else:
    check("page has the stop button setting", False)

print("---- battery lost: fault, opened and latched", flush=True)
t0 = time.time()
wr(400, CMD_SILENT)
check("system goes to fault", wait_for(lambda: rd(204)[0] == 4, 90) is not None, rd(204)[0])
t_fault = time.time() - t0
check("opens about 10 s later", wait_for(lambda: levels() & 7 == 0, 16) is not None, levels())
print(f"     fault after {t_fault:.0f} s, open after {time.time() - t0:.0f} s")
check("engaged 2: fault", rd(REG_ENGAGED)[0] == 2, rd(REG_ENGAGED)[0])
wr(400, CMD_BACK)
time.sleep(12)
check("stays open with the battery back", levels() & 7 == 0 and rd(REG_ENGAGED)[0] == 2, levels())
reboot()
check("closes after a restart", wait_for(closed, 40) is not None, levels())

print("---- hangs, contactors closed each time", flush=True)
for name, hang, cause, max_off_ms in (
        ("core thread spins", HANG_SPIN, RST_WDT_FIRST, 900),
        ("core thread stops", HANG_SLEEP, RST_WDT_FIRST, 900),
        ("main thread stops", HANG_MAIN, RST_WDT_FIRST, 6000),
        ("interrupts locked", HANG_NO_IRQ, RST_WDT_HW, 0),
        ("bus fault", HANG_BUSFAULT, RST_FATAL, 300)):
    assert wait_for(closed, 40) is not None, "did not close"
    before = uptime()
    hang_now(hang)
    took = wait_restart(before, 15)
    check(f"{name}: restarts", took is not None, f"{took:.1f} s" if took else "")
    check(f"{name}: cause", rd(REG_CAUSE)[0] == cause, rd(REG_CAUSE)[0])
    check(f"{name}: no output driven after the reset", rd(REG_OUT_AFTER_RESET)[0] == 0)
    check(f"{name}: outputs open at the start", levels() & 7 == 0, levels())
    if max_off_ms:
        off = rd(412)[0]
        check(f"{name}: outputs off in time", 0 < off <= max_off_ms, f"{off} ms")
    ev = {RST_WDT_FIRST: "RESET_TASK_WDT", RST_WDT_HW: "RESET_INT_WDT", RST_FATAL: "RESET_PANIC"}[cause]
    check(f"{name}: event on the page", wait_for(lambda: ev in web("/events"), 15) is not None, ev)
    reboot()  # one at a time, the counter is tested below

print("---- repeated resets keep the contactors open", flush=True)
for n in range(1, 4):
    assert wait_for(closed, 40) is not None, "did not close"
    before = uptime()
    hang_now(HANG_SPIN)
    assert wait_restart(before, 15) is not None
    check(f"reset {n} in a row counted", rd(REG_IN_ROW)[0] == n, rd(REG_IN_ROW)[0])
check("locked out after the third", rd(REG_LOCKED_OUT)[0] == 1, rd(REG_LOCKED_OUT)[0])
check("no permission to close for batteries that close over CAN", rd(206)[0] == 0, rd(206)[0])
time.sleep(14)
check("stays open", levels() & 7 == 0 and rd(REG_ENGAGED)[0] == 2, (levels(), rd(REG_ENGAGED)[0]))
reboot()
check("a requested restart clears it", rd(REG_LOCKED_OUT)[0] == 0 and rd(REG_IN_ROW)[0] == 0)
check("closes again", wait_for(closed, 40) is not None, levels())

print(f"---- {RESETS} resets of each kind in a row", flush=True)
for hang, cause, name in ((HANG_NO_IRQ, RST_WDT_HW, "hardware"), (HANG_SPIN, RST_WDT_FIRST, "first stage")):
    good = 0
    for n in range(RESETS):
        time.sleep(1.5)
        counted = rd(REG_IN_ROW)[0]
        hang_now(hang)
        if wait_for(lambda: rd(REG_IN_ROW)[0] == counted + 1, 15) is None:
            print(f"     no restart at {n + 1}")
            break
        if rd(REG_CAUSE)[0] != cause or rd(REG_OUT_AFTER_RESET)[0] != 0:
            print(f"     wrong at {n + 1}: cause {rd(REG_CAUSE)[0]}, outputs {rd(REG_OUT_AFTER_RESET)[0]}")
            break
        good += 1
    check(f"{name}: {RESETS} of {RESETS}", good == RESETS, good)

print("---- back to the defaults", flush=True)
reboot()
check("settings at the defaults", settings(r5054=0, r5055=0, r5056=0, r5057=100, r5041=0) == 0)
reboot()

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
