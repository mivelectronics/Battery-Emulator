"""Read a serial port for N seconds and print/save what arrives.
Usage: python serial_log.py COM13 115200 15 out.log
"""
import sys
import time

import serial

port, baud, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
out = sys.argv[4] if len(sys.argv) > 4 else None
buf = bytearray()
with serial.Serial(port, baud, timeout=0.2) as s:
    t0 = time.time()
    while time.time() - t0 < secs:
        buf += s.read(4096)
text = buf.decode("utf-8", "replace")
if out:
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
print(text)
