"""The copies of S32K images on an 8 MB ESP32 (OTA_PLAN.md section 5), on the real boards:
two images sent from files end up as current and previous; the previous one sent again from the ESP32
(no file) is installed and becomes current.

    python ota_store_test.py <esp32 ip> <image A.bin> <image B.bin>

A and B need different versions, both other than the one running. Run with the Python of zw/.venv.
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
results = []
t0 = time.time()


def log(msg):
    print(f"{time.time() - t0:7.1f} {msg}", flush=True)


def check(ok, what):
    results.append((ok, what))
    log(("PASS " if ok else "FAIL ") + what)


def get(ip, path):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=15) as r:
        return json.loads(r.read())


def post(ip, path):
    req = urllib.request.Request(f"http://{ip}{path}", data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def store(ip):
    s = get(ip, "/miv/api/s32k/store")
    return {x["state"]: x for x in s["slots"]}, s


def send_file(ip, image):
    r = subprocess.run([sys.executable, os.path.join(HERE, "ota_web_send.py"), ip, image],
                       capture_output=True, text=True)
    log(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:])
    return r.returncode == 0


def wait_confirm(ip, version, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(2)
        try:
            st = get(ip, "/miv/api/status")
        except Exception:
            continue
        if st.get("s32k_update") == version and st.get("s32k_update_result") in ("installed", "reverted"):
            return st["s32k_update_result"]
    return None


def version_of(path):
    with open(path, "rb") as f:
        h = f.read(32)
    return f"{h[20]}.{h[21]}.{h[22] | h[23] << 8}"


def main():
    ip, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
    va, vb = version_of(a), version_of(b)
    _, s = store(ip)
    check(s["available"], "the ESP32 has the store partitions s32k_a and s32k_b")

    check(send_file(ip, a), f"{va} sent from its file and confirmed")
    time.sleep(4)
    by, _ = store(ip)
    check(by.get("current", {}).get("version") == va, f"{va} kept as current: {list(by)}")

    check(send_file(ip, b), f"{vb} sent from its file and confirmed")
    time.sleep(4)
    by, _ = store(ip)
    check(by.get("current", {}).get("version") == vb and by.get("previous", {}).get("version") == va,
          f"{vb} current, {va} previous")

    prev = by.get("previous")
    if prev:
        code, r = post(ip, f"/miv/api/s32k/store/install?slot={prev['slot']}")
        check(code == 200, f"install of the kept {va} started: {r.get('text') or r.get('phase')}")
        while r.get("phase") in ("starting", "sending", "checking"):
            time.sleep(0.3)
            r = get(ip, "/miv/api/s32k/ota/state")
        log(f"{r.get('phase')} in {r.get('elapsed_ms')} ms, {r.get('repeats')} repeats")
        check(r.get("phase") == "done", f"the S32K took the kept {va}")
        res = wait_confirm(ip, va)
        check(res == "installed", f"{va} from the ESP32's copy confirmed: {res}")
        time.sleep(4)
        by, _ = store(ip)
        check(by.get("current", {}).get("version") == va and by.get("previous", {}).get("version") == vb,
              f"{va} current again, {vb} previous")
    failed = [w for ok, w in results if not ok]
    log(f"{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
