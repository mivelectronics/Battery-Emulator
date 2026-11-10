"""C1 test on the FRDM (variant mb, Modbus on the OpenSDA port), after loading full.hex with a test image in slot 1.

1. first answer after the load: MCUboot has swapped the test image in -> register 18 = 2 (TEST)
2. a setting is written (precharge_ms, register 5057) and stored
3. without the link the test image is not confirmed and restarts itself after 5 minutes;
   MCUboot swaps back -> register 18 = 1 (confirmed), the setting is still there
Prints the time from the start of the script to the first answer and the gap at the revert.
Usage: ota_swap_test.py <port> <precharge value>
"""
import sys
import time

from pymodbus.client import ModbusSerialClient

port, value = sys.argv[1], int(sys.argv[2])
c = ModbusSerialClient(port=port, baudrate=19200, parity="E", stopbits=1, bytesize=8, timeout=0.3)
assert c.connect()
NAMES = {0: "no bootloader", 1: "confirmed", 2: "TEST"}
t0 = time.time()


def log(msg):
    print(f"{time.time() - t0:7.1f} {msg}", flush=True)


def regs(addr, n):
    try:
        r = c.read_holding_registers(addr, count=n, device_id=1)
        return None if r.isError() else r.registers
    except Exception:
        return None


def wr(addr, v):
    r = c.write_register(addr, v, device_id=1)
    return not r.isError()


def state():
    r = regs(0, 19)
    if r is None:
        return None
    return {"uptime": (r[2] << 16) | r[3], "image": r[18], "rev": (r[4] << 16) | r[5], "cause": r[10]}


def wait_answer(limit):
    t = time.time()
    while time.time() - t < limit:
        s = state()
        if s:
            return s, time.time() - t
        time.sleep(0.1)
    return None, limit


s, dt = wait_answer(120)
if not s:
    log("FAIL no answer within 120 s")
    sys.exit(1)
log(f"first answer after {dt:.1f} s: uptime {s['uptime']} s, image {NAMES.get(s['image'], s['image'])}, "
    f"settings rev {s['rev']}")
ok1 = s["image"] == 2
log(("PASS" if ok1 else "FAIL") + " step 2: the test image from slot 1 runs (register 18 = 2)")

before = regs(5057, 1)[0]
assert wr(5000, 0xA5C3) and wr(5057, value) and wr(5001, 1)
time.sleep(0.5)
res = regs(5001, 1)[0]
wr(5000, 0)
s2 = state()
log(f"precharge_ms {before} -> {regs(5057, 1)[0]}, save result {res}, settings rev {s2['rev']}")

last_up = s2["uptime"]
while True:
    s = state()
    if s is None:
        t_lost = time.time()
        log(f"no answer (last uptime {last_up} s): restart")
        s, dt = wait_answer(180)
        if not s:
            log("FAIL no answer within 180 s after the restart")
            sys.exit(1)
        gap = time.time() - t_lost
        log(f"answer again after {gap:.1f} s: uptime {s['uptime']} s, image {NAMES.get(s['image'], s['image'])}, "
            f"settings rev {s['rev']}, last start cause {s['cause']}")
        break
    if s["uptime"] < last_up:
        log(f"uptime fell {last_up} -> {s['uptime']} between polls")
        break
    last_up = s["uptime"]
    if last_up > 420:
        log("FAIL no restart within 420 s")
        sys.exit(1)
    time.sleep(1)

ok3 = s["image"] == 1
log(("PASS" if ok3 else "FAIL") + " step 3: back on the previous image, confirmed (register 18 = 1)")
pv = regs(5057, 1)[0]
ok4 = pv == value
log(("PASS" if ok4 else "FAIL") + f" step 4: the setting survived the two swaps (precharge_ms {pv})")
time.sleep(20)
s = state()
log(f"20 s later: uptime {s['uptime']} s, image {NAMES.get(s['image'], s['image'])}")
sys.exit(0 if ok1 and ok3 and ok4 else 1)
