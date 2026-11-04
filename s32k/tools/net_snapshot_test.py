"""End-to-end test of the data path S32K -> link -> ESP32 -> web page, with the bench simulator.

S32K build: west build ... -- -DBE_SIM=1 -DEXTRA_DTC_OVERLAY_FILE=modbus_on_opensda.overlay
                              -DEXTRA_CONF_FILE=modbus_on_opensda.conf
ESP32 build: env miv_esp32eth_330
Usage: python net_snapshot_test.py COM13 <ESP32 IP>

Reads the truth from the S32K over Modbus RTU and compares it with what the ESP32 serves over Ethernet.
Then checks two batteries in parallel, and that switching the S32K core off restarts the ESP32.
"""
import html
import re
import sys
import time
import urllib.request

from pymodbus.client import ModbusSerialClient

port, ip = sys.argv[1:3]
client = ModbusSerialClient(port=port, baudrate=19200, parity="E", stopbits=1, bytesize=8, timeout=1)
assert client.connect(), "no serial port"
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail != "" else ""))
    failures += 0 if ok else 1


def rd(addr, n=1):
    for _ in range(3):
        r = client.read_holding_registers(addr, count=n, device_id=1)
        if not r.isError():
            return r.registers
        time.sleep(0.2)
    raise RuntimeError(f"read {addr} failed: {r}")


def wr(addr, value):
    r = client.write_register(addr, value, device_id=1)
    assert not r.isError(), f"write {addr}: {r}"


def s16(v):
    return v - 65536 if v >= 32768 else v


def get(path):
    return urllib.request.urlopen(f"http://{ip}{path}", timeout=10).read().decode("utf-8", "replace")


def page_text(path):
    t = re.sub(r"<(script|style).*?</\1>", "", get(path), flags=re.S | re.I)
    t = html.unescape(re.sub(r"<[^>]+>", "\n", t))
    return "\n".join(x.strip() for x in t.splitlines() if x.strip())


def wait_for(name, cond, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(1)
    check(name, False, "timed out")
    return False


def save_and_reboot(settings):
    wr(5000, 0xA5C3)
    for addr, value in settings.items():
        wr(addr, value)
    wr(5001, 1)
    time.sleep(0.5)
    wr(5001, 0xB007)
    time.sleep(3)


# ---- one battery ---------------------------------------------------------------------------------
wait_for("S32K driver found the cells", lambda: rd(1000)[0] == 98)
wait_for("ESP32 shows the battery", lambda: "SOC:" in page_text("/") and "Cell delta: 20 mV" in page_text("/"))
b = rd(1000, 33)
t = page_text("/")
check("protocols named", "Kia/Hyundai 64" in t and "Pylon" in t)
check("page: SOC", f"SOC: {b[10] / 100:.2f}%" in t, b[10])
check("page: SOH", f"SOH: {b[12] / 100:.2f}%" in t, b[12])
check("page: voltage and current", f"Voltage: {b[13] / 10:.1f} V" in t and f"Current: {s16(b[14]) / 10:.1f} A" in t)
check("page: cells", f"Cell min/max: {b[21]} mV / {b[20]} mV" in t, f"{b[21]} / {b[20]}")
check("page: temperatures", f"{s16(b[18]) / 10:.1f}" in t and f"{s16(b[19]) / 10:.1f}" in t)
check("page: 64 kWh", "Total capacity: 64.0 kWh" in t)
check("page: status OK", "System status: OK" in t)
check("page: says commands do not reach the S32K", "do not reach the S32K" in t)

c = get("/cellmonitor")
want = rd(3000, 98)
got = re.findall(r"\b(3[0-9]{3})\b", c)
joined, wanted = ",".join(got), ",".join(str(v) for v in want)
check("cell monitor page holds the 98 voltages", wanted in joined, f"{len(got)} numbers")

ev = page_text("/events")
s32k_events = [e for e in rd(510, 30) if e != 0xFFFF]
check("events page lists the S32K events", "CAN_BATTERY_DETECTED" in ev and len(s32k_events) >= 1)
check("no false memory alarm", "LOW_HEAP_MEMORY" not in ev)

# ---- two batteries ---------------------------------------------------------------------------------
save_and_reboot({5041: 17})
wait_for("ESP32 shows the second battery", lambda: "②" in get("/") and "Total capacity: 128.0 kWh" in page_text("/"))
t = page_text("/")
check("parallel: symbol and combined current", "②" in get("/") and "Current: -10.0 A" in t)
check("parallel: 128 kWh in total", "Total capacity: 128.0 kWh" in t)

# ---- core off: the ESP32 goes back to the local core ---------------------------------------------------
save_and_reboot({5040: 0, 5041: 0, 5042: 0})
wait_for("ESP32 restarted without the battery", lambda: "Kia/Hyundai 64" not in page_text("/"), 40)
check("page no longer names the S32K battery", "Kia/Hyundai 64" not in page_text("/"))
check("notice gone", "do not reach the S32K" not in page_text("/"))

# Back to the simulator defaults
wr(5000, 0xA5C3)
wr(5001, 3)
wr(5001, 1)
wr(5001, 0xB007)

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
