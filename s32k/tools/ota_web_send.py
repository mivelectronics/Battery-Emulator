"""Sends an S32K image through the ESP32, the way the update view of /miv/update does:
POST /miv/api/s32k/ota/begin, chunk (4 KB pieces, again on 503), finish, then GET state until done, then
/miv/api/status until the S32K runs the new image and confirms it.

    python ota_web_send.py <esp32 ip> <zephyr.signed.bin or app_V.bin> [--user U --password P] [--no-wait]
"""
import argparse
import base64
import json
import sys
import time
import urllib.error
import urllib.request

PIECE = 4096


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("image")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--no-wait", action="store_true", help="do not wait for the confirm after the restart")
    a = ap.parse_args()
    with open(a.image, "rb") as f:
        image = f.read()
    headers = {"Content-Type": "application/octet-stream"}
    if a.user:
        headers["Authorization"] = "Basic " + base64.b64encode(f"{a.user}:{a.password}".encode()).decode()
    t0 = time.time()

    def call(method, path, body=None):
        req = urllib.request.Request(f"http://{a.ip}{path}", data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            text = e.read()
            try:
                return e.code, json.loads(text)
            except ValueError:
                return e.code, {"text": text.decode(errors="replace")}

    def log(msg):
        print(f"{time.time() - t0:7.1f} {msg}", flush=True)

    code, s = call("POST", f"/miv/api/s32k/ota/begin?size={len(image)}", image[:32])
    if code != 200:
        log(f"refused: {s.get('text')} ({s.get('error')})")
        return 1
    log(f"update {s.get('version')}, {len(image)} bytes")
    while True:  # BEGIN goes over the link: the S32K checks and stops its core
        code, s = call("GET", "/miv/api/s32k/ota/state")
        if s.get("phase") != "starting":
            break
        time.sleep(0.1)
    if s.get("phase") != "sending":
        log(f"refused by the S32K: {s.get('text')} ({s.get('error')})")
        return 1
    off = 0
    busy = 0
    while off < len(image):
        code, s = call("POST", f"/miv/api/s32k/ota/chunk?offset={off}", image[off:off + PIECE])
        if code == 503:
            busy += 1
            time.sleep(0.05)
            continue
        if code != 200:
            log(f"failed at {off}: {s.get('text')} ({s.get('error')})")
            return 1
        off = s["received"]
        if off % (64 * 1024) < PIECE:
            log(f"{off} bytes sent, {s['written']} written on the S32K")
    code, s = call("POST", "/miv/api/s32k/ota/finish")
    while s.get("phase") in ("sending", "checking"):
        time.sleep(0.2)
        code, s = call("GET", "/miv/api/s32k/ota/state")
    log(f"{s.get('phase')}: {s.get('text') or 'ok'}; {s.get('elapsed_ms')} ms on the ESP32, "
        f"{s.get('repeats')} repeats, {busy} pieces sent again")
    if s.get("phase") != "done":
        return 1
    if a.no_wait:
        return 0
    want = s.get("version")
    seen_test = False
    end = time.time() + 180
    while time.time() < end:
        time.sleep(2)
        try:
            code, st = call("GET", "/miv/api/status")
        except Exception:
            continue
        if code != 200 or st.get("s32k_image") is None:
            continue
        if st.get("s32k_image") == want and st.get("s32k_test_image"):
            if not seen_test:
                log(f"S32K runs {want} as a test image")
            seen_test = True
        if st.get("s32k_update_result") == "installed" and st.get("s32k_image") == want:
            log(f"S32K confirmed {want}")
            return 0
        if st.get("s32k_update_result") == "reverted":
            log(f"S32K reverted the update to {st.get('s32k_update')}, runs {st.get('s32k_image')}")
            return 1
    log("no confirm within 3 minutes")
    return 1


if __name__ == "__main__":
    sys.exit(main())
