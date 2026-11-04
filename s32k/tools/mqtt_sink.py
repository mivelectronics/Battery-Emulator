"""Smallest possible MQTT broker for a bench test: accepts one client, acknowledges what it must and
prints what it publishes. No dependencies. Usage: python mqtt_sink.py <seconds> [port]
"""
import json
import socket
import sys
import time

secs = float(sys.argv[1])
port = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(secs)
print("listening on", port, flush=True)
try:
    con, addr = srv.accept()
except socket.timeout:
    print("no client connected")
    sys.exit(1)
print("client", addr[0], flush=True)
con.settimeout(0.5)
buf = b""
topics = {}
t0 = time.time()
while time.time() - t0 < secs:
    try:
        d = con.recv(8192)
        if not d:
            break
        buf += d
    except socket.timeout:
        pass
    while len(buf) >= 2:
        i, mult, length = 1, 1, 0
        complete = False
        while i < len(buf):
            length += (buf[i] & 127) * mult
            mult *= 128
            i += 1
            if not buf[i - 1] & 128:
                complete = True
                break
        if not complete or len(buf) < i + length:
            break
        kind, flags, body = buf[0] >> 4, buf[0] & 15, buf[i:i + length]
        buf = buf[i + length:]
        if kind == 1:  # CONNECT
            con.sendall(bytes([0x20, 2, 0, 0]))
        elif kind == 8:  # SUBSCRIBE
            con.sendall(bytes([0x90, 3, body[0], body[1], 0]))
        elif kind == 12:  # PINGREQ
            con.sendall(bytes([0xD0, 0]))
        elif kind == 3:  # PUBLISH
            n = (body[0] << 8) | body[1]
            topic = body[2:2 + n].decode("utf-8", "replace")
            pos = 2 + n
            qos = (flags >> 1) & 3
            if qos:
                con.sendall(bytes([0x40, 2, body[pos], body[pos + 1]]))
                pos += 2
            topics[topic] = body[pos:].decode("utf-8", "replace")
print("topics:", len(topics))
for k in sorted(topics):
    if "config" in k or "discovery" in k:
        continue
    print(k, "=", topics[k][:1500])
