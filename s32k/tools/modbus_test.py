"""Modbus RTU test of the S32K emulator (MODBUS_MAP.md).

Usage: python modbus_test.py COM13 [baud] [unit]
With the modbus_on_opensda build the server sits on the OpenSDA virtual COM port.
Checks: reads system and CAN registers, refuses writes while locked, unlocks, changes the CAN1
data bit rate, saves to NVS, verifies, and restores the original value.
"""
import sys
import time

from pymodbus.client import ModbusSerialClient

port = sys.argv[1]
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 19200
unit = int(sys.argv[3]) if len(sys.argv) > 3 else 1
failures = 0


def check(cond, text):
    global failures
    print(("PASS " if cond else "FAIL ") + text)
    if not cond:
        failures += 1


def rd(addr, n=1):
    r = c.read_holding_registers(addr, count=n, device_id=unit)
    if r.isError():
        raise RuntimeError(f"read {addr}: {r}")
    return r.registers


def wr(addr, value):
    return c.write_register(addr, value, device_id=unit)


c = ModbusSerialClient(port=port, baudrate=baud, parity="E", stopbits=1, bytesize=8, timeout=1)
assert c.connect(), "cannot open " + port

sysregs = rd(0, 10)
print("system:", sysregs)
check(sysregs[0] == 1, "map version 1")
up1 = sysregs[2] << 16 | sysregs[3]
time.sleep(1.2)
s2 = rd(2, 2)
check((s2[0] << 16 | s2[1]) > up1, f"uptime advances ({up1} s)")

for i, base in enumerate((100, 120, 140)):
    regs = rd(base, 11)
    print(f"CAN{i}: state {regs[0]} tec {regs[1]} rec {regs[2]} rx {regs[3] << 16 | regs[4]} "
          f"{regs[9]}/{regs[10]} kbit/s")
    check(regs[9] in (100, 125, 200, 250, 500, 800, 1000), f"CAN{i} running")

cfg = rd(5000, 43)
orig = cfg[15]  # 5015: CAN1 data bit rate
print("settings:", cfg[10:13], cfg[14:17], cfg[18:21], "modbus", cfg[30:33])

r = wr(5015, 1000)
check(r.isError(), "write refused while locked")

check(not wr(5000, 0xA5C3).isError(), "unlock")
check(rd(5000)[0] == 1, "unlocked flag")
new = 1000 if orig != 1000 else 2000
check(not wr(5015, new).isError(), f"stage CAN1 data rate {new}")
check(rd(9)[0] == 1, "unsaved edits flagged")
rev_before = rd(4, 2)
check(not wr(5001, 1).isError(), "command save+apply")
time.sleep(0.5)
check(rd(5001)[0] == 0, "save result 0")
rev_after = rd(4, 2)
check((rev_after[0] << 16 | rev_after[1]) == (rev_before[0] << 16 | rev_before[1]) + 1, "revision +1")
check(rd(120 + 10)[0] == new, "CAN1 runs at the new data rate")

check(not wr(5011, 7777).isError(), "stage an invalid value")
wr(5001, 1)
time.sleep(0.3)
check(rd(5001)[0] == (65536 - 22), "invalid config refused (-EINVAL)")
wr(5001, 2)  # discard

wr(5015, orig)
wr(5001, 1)
time.sleep(0.5)
check(rd(5015)[0] == orig and rd(130)[0] == orig, f"restored CAN1 data rate {orig}")
wr(5000, 0)
check(rd(5000)[0] == 0, "locked again")

c.close()
print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
sys.exit(1 if failures else 0)
