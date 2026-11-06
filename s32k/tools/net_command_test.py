"""End-to-end test of the path ESP32 -> link -> S32K, with the bench simulator.

Build as for net_snapshot_test.py. Usage: python net_command_test.py COM13 <ESP32 IP>

Acts through the web server of the ESP32 like a browser does, and reads the result from the S32K over
Modbus RTU. Leaves the S32K and ESP32 settings as it found them.
"""
import html
import re
import sys
import time
import urllib.parse
import urllib.request

from pymodbus.client import ModbusSerialClient

port, ip = sys.argv[1:3]
client = ModbusSerialClient(port=port, baudrate=19200, parity="E", stopbits=1, bytesize=8, timeout=1)
assert client.connect(), "no serial port"
failures = 0

REG_AGG_CHARGE_POWER = 213
REG_CFG_MAX_CHARGE = 5052
REG_KV_COUNT = 225
REG_OTHER_UDS = 410
REG_NETWORK_KEYS = 411
REG_KV_REVISION = 413


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail != "" else ""))
    failures += 0 if ok else 1


def rd(addr, n=1):
    for _ in range(4):
        try:
            r = client.read_holding_registers(addr, count=n, device_id=1)
            if not r.isError():
                return r.registers
        except Exception:
            pass
        time.sleep(0.5)
    raise RuntimeError(f"read {addr} failed")


def s16(v):
    return v - 65536 if v >= 32768 else v


def web(path, data=None):
    req = urllib.request.Request(f"http://{ip}{path}", data=data)
    return urllib.request.urlopen(req, timeout=15).read().decode("utf-8", "replace")


def wait_for(name, cond, secs=20):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond():
                check(name, True, f"{time.time() - t0:.1f} s")
                return True
        except Exception:
            pass
        time.sleep(0.5)
    check(name, False, "timed out")
    return False


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
            data[n.group(1)] = html.unescape(v.group(1)) if v else ""
    for m in re.finditer(r"<select\b[^>]*name=['\"]([^'\"]+)[^>]*>(.*?)</select>", form, re.I | re.S):
        o = re.search(r"<option[^>]*value=['\"]([^'\"]*)['\"][^>]*selected", m.group(2), re.I)
        if not o:
            o = re.search(r"<option[^>]*selected[^>]*value=['\"]([^'\"]*)", m.group(2), re.I)
        if o:
            data[m.group(1)] = o.group(1)
    return data


def save_form(data):
    web("/saveSettings", urllib.parse.urlencode(data).encode())


def active_event_ids():
    return [e for e in rd(510, 40) if e != 0xFFFF]


def event_name_active(name):
    return name in web("/events")


wait_for("ESP32 shows the S32K battery", lambda: "Cell delta: 20 mV" in re.sub(r"<[^>]+>", "\n", web("/")))
wait_for("S32K driver found the cells", lambda: rd(1000)[0] == 98, 60)
time.sleep(3)

# ---- pause ------------------------------------------------------------------------------------------
before = (rd(REG_AGG_CHARGE_POWER, 2)[0] << 16) | rd(REG_AGG_CHARGE_POWER + 1)[0]
check("charge limit above zero before the pause", before > 0, before)
web("/pause?value=true")
wait_for("pause: S32K takes the charge limit to zero", lambda: rd(REG_AGG_CHARGE_POWER, 2) == [0, 0])
wait_for("pause: event on the page", lambda: "PAUSE_BEGIN" in web("/events"))
web("/pause?value=false")
wait_for("resume: S32K gives the limit back", lambda: rd(REG_AGG_CHARGE_POWER, 2) != [0, 0], 40)

# ---- equipment stop, kept over a restart ------------------------------------------------------------
web("/equipmentStop?value=1")
wait_for("equipment stop: S32K takes the charge limit to zero", lambda: rd(REG_AGG_CHARGE_POWER, 2) == [0, 0])
wait_for("equipment stop: event on the page", lambda: "EQUIPMENT_STOP" in web("/events"))
r = client.write_register(5000, 0xA5C3, device_id=1)
client.write_register(5001, 0xB007, device_id=1)
time.sleep(5)
wait_for("equipment stop survives a restart of the S32K", lambda: rd(REG_AGG_CHARGE_POWER, 2) == [0, 0] and rd(200)[0] == 1)
time.sleep(4)
check("still stopped on the page after the restart", "EQUIPMENT_STOP" in web("/events"))
web("/equipmentStop?value=0")
wait_for("resume after equipment stop", lambda: rd(REG_AGG_CHARGE_POWER, 2) != [0, 0], 40)

# ---- a battery command through the stand-in object ---------------------------------------------------
adv = web("/advanced")
check("advanced page offers the driver's Read DTC button", "readDTC" in adv)
n = rd(REG_OTHER_UDS)[0]
req = urllib.request.Request(f"http://{ip}/readDTC", data=b"0", method="PUT", headers={"Content-Type": "text/plain"})
check("Read DTC accepted", urllib.request.urlopen(req, timeout=10).status == 200)
wait_for("Read DTC: the driver on the S32K asks the battery over UDS", lambda: rd(REG_OTHER_UDS)[0] > n)

# ---- live setting through the shadow of battery_settings, stored too ----------------------------------
was = rd(REG_CFG_MAX_CHARGE)[0]
web("/updateMaxChargeA?value=12.5")
wait_for("max charge current reaches the running S32K", lambda: rd(217)[0] == 125)
wait_for("and is stored in its settings", lambda: rd(REG_CFG_MAX_CHARGE)[0] == 125)
web(f"/updateMaxChargeA?value={was / 10}")
wait_for("back to the value before", lambda: rd(REG_CFG_MAX_CHARGE)[0] == was)

# ---- settings form: be_cfg key, plain key, network key ------------------------------------------------
form = settings_form()
original = dict(form)
kv_changes = rd(REG_KV_REVISION)[0]
check("settings page reads the S32K battery type", form.get("battery") == str(rd(5040)[0]), form.get("battery"))
# Other values than the ones there: an earlier run may have left them stored
form["PYLONSEND"] = "2" if form.get("PYLONSEND") == "3" else "3"
form["HOSTNAME"] = "be-test"
form["INVCAPACITY"] = "124" if form.get("INVCAPACITY") == "123" else "123"
save_form(form)
wait_for("plain settings reach the S32K", lambda: ((rd(REG_KV_REVISION)[0] - kv_changes) & 0xFFFF) >= 2)
time.sleep(1)
kv_now = rd(REG_KV_COUNT)[0]
check("network settings stay on the ESP32", rd(REG_NETWORK_KEYS)[0] == 0, f"{rd(REG_NETWORK_KEYS)[0]} of {kv_now} keys")
tick = rd(222)[0]
print(f"     longest core tick after saving the form: {tick} us")
check("flash writes do not stall the core beyond 10 ms", tick < 10000, tick)

# A combination the S32K cannot run: refused, and the page shows what it runs with
form = settings_form()
check("settings page shows the battery channel", form.get("BATTCOMM") == "6", form.get("BATTCOMM"))
form["BATTCOMM"] = "3"  # CAN0, where the inverter is
save_form(form)
time.sleep(1.5)
wait_for("invalid combination refused, page back to the real channel", lambda: settings_form().get("BATTCOMM") == "6", 15)
check("S32K kept its channel", rd(5043)[0] == 1, rd(5043)[0])

# Modbus to the page: the S32K holds the truth
client.write_register(5000, 0xA5C3, device_id=1)
client.write_register(5044, 0, device_id=1)  # battery 2 channel; not in use, so not refused
client.write_register(5001, 1, device_id=1)
client.write_register(5000, 0, device_id=1)
wait_for("change over Modbus shows on the settings page", lambda: settings_form().get("BATT2COMM") == "3", 15)
client.write_register(5000, 0xA5C3, device_id=1)
client.write_register(5044, 2, device_id=1)
client.write_register(5001, 1, device_id=1)
client.write_register(5000, 0, device_id=1)
# The next form must not carry the old channel back to the S32K
wait_for("and back", lambda: settings_form().get("BATT2COMM") != "3", 15)

# Battery type from the form takes effect after the restart
form = settings_form()
form["battery"] = "34"  # the fake battery
save_form(form)
wait_for("battery type reaches the S32K settings", lambda: rd(5040)[0] == 34)
up = (rd(2, 2)[0] << 16) | rd(3)[0]
web("/reboot")
wait_for("Reboot Emulator restarts the S32K too", lambda: ((rd(2, 2)[0] << 16) | rd(3)[0]) < up, 30)
wait_for("S32K runs the new battery type", lambda: rd(201)[0] == 34, 15)
time.sleep(8)
wait_for("ESP32 is back and names it", lambda: "Fake" in web("/") or "fake" in web("/"), 40)
form = settings_form()
form["battery"] = "17"
save_form(form)
wait_for("back to Kia/Hyundai 64 in the settings", lambda: rd(5040)[0] == 17)

# ---- restart of both from the page ------------------------------------------------------------------
up = (rd(2, 2)[0] << 16) | rd(3)[0]
web("/reboot")
wait_for("Reboot Emulator restarts the S32K too", lambda: ((rd(2, 2)[0] << 16) | rd(3)[0]) < up, 30)
time.sleep(10)
wait_for("ESP32 is back and shows the data", lambda: "Cell delta" in web("/"), 40)
check("settings survived on the S32K", rd(REG_KV_COUNT)[0] == kv_now, rd(REG_KV_COUNT)[0])
check("back on the battery type from the form", rd(201)[0] == 17, rd(201)[0])

# Leave things as found
original["HOSTNAME"] = original.get("HOSTNAME", "")
save_form(original)
time.sleep(1)
client.write_register(5000, 0xA5C3, device_id=1)
client.write_register(5001, 0xB007, device_id=1)

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
