"""End-to-end test of the Battery-Emulator core on the S32K, with the bench simulator (BE_SIM).

Build: west build ... -- -DBE_SIM=1 -DEXTRA_DTC_OVERLAY_FILE=modbus_on_opensda.overlay
                        -DEXTRA_CONF_FILE=modbus_on_opensda.conf
Usage: python core_sim_test.py COM13 <path to 17_Kia64_UDSpolls.txt>

The simulated battery sends known values on real CAN channels in internal loopback and answers the
UDS polls with a capture from a real battery. Everything is read back over Modbus RTU, up to the frame
the inverter driver sends, so this checks the path CAN -> driver -> datalayer -> safety -> inverter.
Also checks settings through NVS: second battery in parallel, and the core switched off.
"""
import re
import sys
import time

from pymodbus.client import ModbusSerialClient

port, log = sys.argv[1:3]
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


def u32(hi, lo):
    return (hi << 16) | lo


def expected_cells():
    """The cell voltages the driver must extract from the capture."""
    groups, pid = {}, None
    for line in open(log):
        m = re.search(r"(TX5|rx4) (7e4|7ec) \[8\] ((?:[0-9a-f]{2} ?){8})", line)
        if not m:
            continue
        d = [int(x, 16) for x in m.group(3).split()]
        if m.group(2) == "7e4":
            if d[0] == 0x03 and d[1] == 0x22:
                pid = (d[2] << 8) | d[3]
                groups[pid] = []
        elif pid is not None:
            groups[pid] += d[5:8] if (d[0] & 0xF0) == 0x10 else d[1:8]
    cells = {}
    for pid, base in ((0x102, 0), (0x103, 32), (0x104, 64)):
        for i in range(32):
            cells[base + i] = groups[pid][4 + i] * 20
    cells[96] = groups[0x105][34] * 20
    cells[97] = groups[0x105][35] * 20
    return {k: v for k, v in cells.items() if v > 4 * 20}


def wait_for(name, cond, secs=30):
    t0 = time.time()
    while time.time() - t0 < secs:
        if cond():
            return True
        time.sleep(1)
    check(name, False, "timed out")
    return False


def save_and_reboot(settings):
    wr(5000, 0xA5C3)
    for addr, value in settings.items():
        wr(addr, value)
    wr(5001, 1)
    time.sleep(0.5)
    check("settings saved", s16(rd(8)[0]) == 0, f"result {s16(rd(8)[0])}")
    wr(5001, 0xB007)
    time.sleep(3)


# ---- one battery, one inverter (the BE_SIM defaults) -----------------------------------------------
check("map version 2", rd(0)[0] == 2)
check("core running", rd(200)[0] == 1)
check("battery type Kia/Hyundai 64", rd(201)[0] == 17)
check("inverter type Pylon", rd(202)[0] == 10)
check("simulator present", rd(400)[0] == 1)

wait_for("driver found 98 cells", lambda: rd(1000)[0] == 98, 60)
b = rd(1000, 33)
check("cells", b[0] == 98, b[0])
check("capacity 64 kWh", u32(b[1], b[2]) == 64000, u32(b[1], b[2]))
check("SOC 65.00 % from 0x542", b[10] == 6500, b[10])
check("voltage 350.9 V from 0x595", b[13] == 3509, b[13])
check("current -5.0 A from 0x595", s16(b[14]) == -50, s16(b[14]))
check("temperature min 20 C from 0x596", s16(b[18]) == 200, s16(b[18]))
check("temperature max 25 C from 0x596", s16(b[19]) == 250, s16(b[19]))
check("SOH from UDS group 5", 5000 < b[12] <= 10000, b[12])
check("cell max from UDS group 1", b[20] == 0xB3 * 20, b[20])
check("cell min from UDS group 1", b[21] == 0xB2 * 20, b[21])
check("BMS alive counter", b[28] <= 60, b[28])

cells = rd(3000, 98)
want = expected_cells()
bad = [i for i, v in want.items() if cells[i] != v]
check(f"{len(want)} cell voltages match the capture", not bad, f"wrong: {bad[:8]}")

st = rd(200, 25)
check("system status active", st[4] == 3, st[4])
check("inverter seen by the driver", st[8] > 0, st[8])
ev = rd(500, 30)
active = [e for e in ev[10:] if e != 0xFFFF]
print("     active events:", active, "level", ev[2], "of", ev[0])
check("no warning or error events", ev[2] <= 1, f"level {ev[2]}")

# What goes to the inverter, read from the frame itself: 0x4210 = voltage, current (+30000 offset off)
wait_for("inverter frames", lambda: rd(409)[0] > 3)
f = rd(401, 8)
volt, curr = (f[0] << 8) | f[1], s16((f[2] << 8) | f[3])
check("0x4210 to the inverter: voltage 350.9 V", volt == 3509, volt)
check("0x4210 to the inverter: current -5.0 A", curr == -50, curr)
check("aggregate SOC", st[10] == 6500, st[10])

heap, tick, drops, stack = rd(221, 4)
print(f"     free heap {heap} B, longest tick {tick} us, CAN frames dropped {drops}, stack free {stack} B")
check("no CAN frames dropped", drops == 0, drops)
check("a tick stays under 1 ms", tick < 1000, tick)

# ---- battery and inverter on one bus, the channel with the transceiver on the board -------------------
save_and_reboot({5043: 0})
check("one bus: battery on the channel of the inverter", rd(5043)[0] == rd(5045)[0] == 0)
wait_for("one bus: driver found the cells", lambda: rd(1000)[0] == 98)
b = rd(1000, 33)
check("one bus: SOC", b[10] == 6500, b[10])
check("one bus: voltage", b[13] == 3509, b[13])
wait_for("one bus: system active", lambda: rd(204)[0] == 3)
check("one bus: inverter seen by the driver", rd(208)[0] > 0, rd(208)[0])
wait_for("one bus: frames to the inverter", lambda: rd(409)[0] > 3)
f = rd(401, 8)
volt, curr = (f[0] << 8) | f[1], s16((f[2] << 8) | f[3])
check("one bus: 0x4210 voltage", volt == 3509, volt)
check("one bus: 0x4210 current", curr == -50, curr)
check("one bus: no warning or error events", rd(502)[0] <= 1, rd(502)[0])
heap, tick, drops, stack = rd(221, 4)
print(f"     longest tick {tick} us, CAN frames dropped {drops}")
check("one bus: no CAN frames dropped", drops == 0, drops)
check("one bus: a tick stays under 1 ms", tick < 1000, tick)
# Two batteries of a type send the same IDs
wr(5000, 0xA5C3)
wr(5041, 17)
wr(5044, 0)
wr(5001, 1)
time.sleep(0.5)
check("one bus: a second battery on it is refused", s16(rd(8)[0]) != 0, s16(rd(8)[0]))
wr(5001, 2)
save_and_reboot({5043: 1})

# ---- second battery in parallel, through NVS and a reboot --------------------------------------------
save_and_reboot({5041: 17})
check("second battery on", rd(203)[0] == 1)
wait_for("second battery found 98 cells", lambda: rd(2000)[0] == 98, 60)
b2 = rd(2000, 33)
check("battery 2 SOC", b2[10] == 6500, b2[10])
check("battery 2 voltage", b2[13] == 3509, b2[13])
check("battery 2 current", s16(b2[14]) == -50, s16(b2[14]))
check("battery 2 cell voltages", rd(3500)[0] == cells[0], rd(3500)[0])
time.sleep(3)
st = rd(200, 25)
check("parallel: current adds up to -10.0 A", s16(st[12]) == -100, s16(st[12]))
check("parallel: charge limit from both packs", u32(st[13], st[14]) > 0, u32(st[13], st[14]))
heap, tick, drops, stack = rd(221, 4)
print(f"     free heap {heap} B, longest tick {tick} us, CAN frames dropped {drops}, stack free {stack} B")
check("parallel: no CAN frames dropped", drops == 0, drops)
for ch, base in (("CAN0", 100), ("CAN1", 120), ("CAN4", 140)):
    c = rd(base, 13)
    print(f"     {ch}: rx {u32(c[3], c[4])} tx {u32(c[5], c[6])} tx errors {u32(c[7], c[8])} queue drops {u32(c[11], c[12])}")
    check(f"{ch}: no frames dropped from the transmit queue", u32(c[11], c[12]) == 0)
check("parallel: a tick stays under 1 ms", tick < 1000, tick)

# ---- core off: the channels go back to the link ------------------------------------------------------
save_and_reboot({5040: 0, 5041: 0, 5042: 0})
check("core off with battery type 0", rd(200)[0] == 0)
check("Modbus still answers without the core", rd(0)[0] == 2)

# Back to the defaults for the next run
wr(5000, 0xA5C3)
wr(5001, 3)
wr(5001, 1)
wr(5001, 0xB007)

print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
