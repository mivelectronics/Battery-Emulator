"""The be_link protocol (Software/src/communication/link/be_link.h) in Python, for tests where a PC takes the
place of the ESP32 on the link: frames, HELLO, acks, the SNAP_S32K status and the image update (BE_LINK_OTA).
"""
import hashlib
import struct
import threading
import time

import serial

HELLO, SNAPSHOT, SETTINGS, CMD, CAN, EVENT, ACK, LOG, OTA = range(1, 10)
ROLE_NET = 2
PROTO_VERSION = 1
SNAP_S32K = 7
CMD_PAUSE = 11

OTA_BEGIN, OTA_DATA, OTA_FINISH, OTA_ABORT = 1, 2, 3, 4
OTA_DATA_MAX = 1000
OTA_ERRORS = {
    -200: "NO_BOOTLOADER", -201: "NOT_IMAGE", -202: "TOO_BIG", -203: "FLAGS", -204: "CONTACTORS",
    -205: "NOT_PAUSED", -206: "CURRENT", -207: "TEST_IMAGE", -208: "SEQUENCE", -209: "FLASH", -210: "HASH",
    -211: "VECTORS", -212: "BOARD", -213: "TLV", -214: "MARK", -16: "EBUSY",
}

F_WDT, F_LOCKOUT, F_CORE, F_TEST, F_UPDATING, F_INSTALLED, F_REVERTED = 1, 2, 4, 8, 0x10, 0x20, 0x40


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data):
    out = bytearray([0])
    code_pos, code = 0, 1
    for b in data:
        if b == 0:
            out[code_pos] = code
            code_pos, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_pos] = code
                code_pos, code = len(out), 1
                out.append(0)
    out[code_pos] = code
    return bytes(out)


def cobs_decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0:
            return None
        i += 1
        chunk = data[i:i + code - 1]
        if len(chunk) != code - 1:
            return None
        out += chunk
        i += code - 1
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode(ftype, seq, payload):
    raw = struct.pack("<BBH", ftype, seq & 0xFF, len(payload)) + payload
    raw += struct.pack("<H", crc16(raw))
    return b"\x00" + cobs_encode(raw) + b"\x00"


def parse_status(p):
    """be_s32k_status after the 4 byte section header of SNAP_S32K"""
    s = p[4:]
    if len(s) < 164:
        return None
    return {
        "reset_cause": s[0], "flags": s[2], "outputs": struct.unpack_from("<H", s, 4)[0],
        "uptime": struct.unpack_from("<I", s, 8)[0],
        "fw": s[20:44].split(b"\0")[0].decode(), "image": s[116:140].split(b"\0")[0].decode(),
        "update": s[140:164].split(b"\0")[0].decode(),
    }


class Link:
    """A PC in place of the ESP32: HELLO every second, frames parsed on a thread."""

    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.seq = 0
        self.lock = threading.Lock()
        self.cv = threading.Condition()
        self.acks = []
        self.status = None
        self.status_time = 0.0
        self.restarts = 0
        self.restart_gap = 0.0
        self.peer_hello = 0.0
        self.peer_fw = ""
        self.hello_on = True
        self.logs = []
        self.stats = {"frames": 0, "bad": 0}
        self.stop = False
        threading.Thread(target=self._rx, daemon=True).start()
        threading.Thread(target=self._hello, daemon=True).start()

    def close(self):
        self.stop = True
        time.sleep(0.2)
        self.ser.close()

    def send(self, ftype, payload):
        with self.lock:
            self.seq = (self.seq + 1) & 0xFF
            self.ser.write(encode(ftype, self.seq, payload))

    def _hello(self):
        while not self.stop:
            if self.hello_on:
                fw = b"pc-ota-test".ljust(24, b"\0")
                self.send(HELLO, struct.pack("<HBBII", PROTO_VERSION, ROLE_NET, 0, 0, 0) + fw)
            time.sleep(1.0)

    def _frame(self, raw):
        if len(raw) < 6 or crc16(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
            self.stats["bad"] += 1
            return
        ftype, _seq, n = struct.unpack_from("<BBH", raw)
        p = raw[4:4 + n]
        self.stats["frames"] += 1
        if ftype == HELLO:
            self.peer_hello = time.time()
            self.peer_fw = p[12:36].split(b"\0")[0].decode(errors="replace")
        elif ftype == SNAPSHOT and p and p[0] == SNAP_S32K:
            st = parse_status(p)
            if st:
                now = time.time()
                if self.status and st["uptime"] + 2 < self.status["uptime"] + (now - self.status_time):
                    # Uptime is behind the time since the last status: the S32K restarted in between.
                    # Gap = silence between the two statuses
                    self.restarts += 1
                    self.restart_gap = now - self.status_time
                self.status, self.status_time = st, now
        elif ftype == ACK and len(p) >= 4:
            with self.cv:
                self.acks.append(p)
                self.cv.notify_all()
        elif ftype == LOG:
            self.logs.append(p.decode(errors="replace"))

    def _rx(self):
        buf = bytearray()
        while not self.stop:
            try:
                d = self.ser.read(4096)
            except Exception:
                return
            for b in d:
                if b == 0:
                    if buf:
                        raw = cobs_decode(bytes(buf))
                        if raw is None:
                            self.stats["bad"] += 1
                        else:
                            self._frame(raw)
                    buf.clear()
                else:
                    buf.append(b)

    def wait_ack(self, acked_type, acked_seq, timeout):
        end = time.time() + timeout
        with self.cv:
            while True:
                for i, p in enumerate(self.acks):
                    if p[0] == acked_type and p[1] == acked_seq:
                        del self.acks[: i + 1]
                        return p
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def peer_alive(self):
        return time.time() - self.peer_hello < 3.0

    def wait_peer(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.peer_alive():
                return True
            time.sleep(0.1)
        return False

    def wait_status(self, timeout, pred=lambda s: True, newer_than=0.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.status and self.status_time > newer_than and pred(self.status):
                return self.status
            time.sleep(0.1)
        return None

    def cmd(self, code, arg0=0, arg1=0):
        # The S32K takes a command id once (a repeat is only acknowledged): a new id every time
        self.cmd_id = self.cmd_id % 0xFFFF + 1 if hasattr(self, "cmd_id") else 0x7701
        cmd_id = self.cmd_id
        self.send(CMD, struct.pack("<HHII", code, cmd_id, arg0, arg1))
        return self.wait_ack(CMD, cmd_id & 0xFF, 2.0)


class OtaSender:
    """One update over the link, one frame in flight, repeated until acknowledged (as the ESP32 does)."""

    def __init__(self, link, image, image_id=None, retry_s=0.2, tries=25):
        self.link, self.image = link, image
        self.image_id = image_id if image_id is not None else int(time.time()) & 0xFFFFFFFF
        self.retry_s, self.tries = retry_s, tries
        self.fid = 0
        self.repeats = 0

    def _xfer(self, op, offset=0, extra=b""):
        self.fid = (self.fid + 1) & 0xFF
        frame = struct.pack("<BBHIII", op, self.fid, 0, self.image_id, len(self.image), offset) + extra
        for attempt in range(self.tries):
            if attempt:
                self.repeats += 1
            self.link.send(OTA, frame)
            a = self.link.wait_ack(OTA, self.fid, self.retry_s if op != OTA_FINISH else 5.0)
            if a is not None:
                status = struct.unpack_from("<h", a, 2)[0]
                written = struct.unpack_from("<I", a, 8)[0] if len(a) >= 12 else 0
                return status, written
        return None, None

    def begin(self, sha=None, head=None):
        sha = sha if sha is not None else hashlib.sha256(self.image).digest()
        head = head if head is not None else self.image[:32]
        return self._xfer(OTA_BEGIN, 0, sha + head.ljust(32, b"\0"))

    def data_all(self, progress=None, corrupt_at=None):
        off = 0
        while off < len(self.image):
            chunk = self.image[off:off + OTA_DATA_MAX]
            if corrupt_at is not None and off <= corrupt_at < off + len(chunk):
                b = bytearray(chunk)
                b[corrupt_at - off] ^= 0xFF
                chunk = bytes(b)
            status, written = self._xfer(OTA_DATA, off, chunk)
            if status != 0:
                return status, off
            off = written
            if progress:
                progress(off)
        return 0, off

    def finish(self):
        return self._xfer(OTA_FINISH)

    def abort(self):
        return self._xfer(OTA_ABORT)


def err_name(status):
    return "ok" if status == 0 else ("no answer" if status is None else OTA_ERRORS.get(status, str(status)))
