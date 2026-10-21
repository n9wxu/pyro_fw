#!/usr/bin/env python3
"""
failed_write_check.py -- an upload the storage cannot finish, on a live board.

Once the cleanup is done, uploads a file larger than the room the board
reports, which the board refuses part-way. Then, without a restart: the part
file is gone, and a delete of the name finds no file. A small upload of the
same name while the cleanup erases the upload's pages is refused for it. The
cleanup, on USB with test mode off, erases them one sector after another until
/api/fs says it is done: at least five sectors a second, five times its pace
off USB. Then the small upload is taken, and reads back whole. The board's
uptime runs on throughout, and the sensor's readings beside the flash writes
and the erases keep the pad's sensor check satisfied.

Verifies on hardware [WEB-API-18, WEB-API-21, SNS-PRES-14, USB-10].

Waits up to CLEAN_WAIT_S for the cleanup: about a minute and a half for 4 MB.
Refuses to run on a board that is not in PAD_IDLE.

Usage:
    support/failed_write_check.py 192.168.71.1

SPDX-License-Identifier: MIT
"""
import http.client
import json
import os
import sys
import time
import urllib.error
import urllib.request

HOST = sys.argv[1]
NAME = "/www/failed_write_check.bin"
CHUNK = 64 * 1024
CLEAN_WAIT_S = 600
FAST_PER_S = 5.0  # sectors a second, five times the pace off USB (USB-10)
results = []


def req(method, path, body=None, timeout=10):
    r = urllib.request.Request(f"http://{HOST}{path}", data=body, method=method)
    if method == "POST":
        r.add_header("X-Pyro", "1")
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def status():
    return json.loads(req("GET", "/api/status")[1])


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""))


def upload_too_big(size):
    """The status the board answers, or None when it drops the connection."""
    c = http.client.HTTPConnection(HOST, timeout=60)
    c.putrequest("POST", NAME)
    c.putheader("X-Pyro", "1")
    c.putheader("Content-Length", str(size))
    c.endheaders()
    sent = 0
    try:
        while sent < size:
            n = min(CHUNK, size - sent)
            c.send(os.urandom(n))
            sent += n
    except OSError:
        pass  # the board answered and closed
    try:
        return c.getresponse().status, sent
    except (OSError, http.client.HTTPException):
        return None, sent
    finally:
        c.close()


def cleanup_done(secs=CLEAN_WAIT_S):
    """/api/fs once it reports the cleanup done, or the last report."""
    fs, t = {}, time.time()
    while time.time() - t < secs:
        fs = json.loads(req("GET", "/api/fs")[1])
        if fs.get("cleanup") == "done":
            break
        time.sleep(2)
    return fs


st0 = status()
if st0["state"] != "PAD_IDLE":
    sys.exit(f"{HOST} is in {st0['state']}; run this on the pad only")
print(f"== {st0['board']} {st0['fw_version']} at {HOST}")
cleanup_done()
space = json.loads(req("GET", "/api/log/space")[1])
size = space["bytes_free"] + 256 * 1024
print(f"room {space['bytes_free']} bytes; uploading {size}")
t0 = time.time()
code, sent = upload_too_big(size)
check("an upload past the room is refused", code == 500, f"{code} after {sent} bytes, {time.time() - t0:.0f} s")

code, _ = req("GET", NAME + ".part")
check("its part file is gone", code == 404, str(code))
code, _ = req("GET", NAME)
check("no file of its name", code == 404, str(code))
code, _ = req("POST", "/api/delete" + NAME)
check("a delete of its name finds none", code == 404, str(code))

small = os.urandom(1024)
code, body = req("POST", NAME, small)
erasing = code == 503 and json.loads(body).get("erasing") is True
check("a small upload of its name while the cleanup erases is refused for it", erasing,
      f"{code} {body[:60]!r}")

e0, t0 = status()["flash_erases"], time.time()
fs = cleanup_done()
erased, took = status()["flash_erases"] - e0, time.time() - t0
check("the cleanup finishes on USB", fs.get("cleanup") == "done", f"{fs.get('cleanup')} after {took:.0f} s")
check(f"at {FAST_PER_S:g} sectors a second or more (USB-10)", erased >= FAST_PER_S * max(took - 2, 1),
      f"{erased} sectors in {took:.0f} s")

code, _ = req("POST", NAME, small)
check("then the small upload is taken", code == 201, str(code))
code, body = req("GET", NAME)
check("and reads back whole", code == 200 and body == small, f"{code} {len(body)} bytes")
code, _ = req("POST", "/api/delete" + NAME)
check("and is deleted", code == 200, str(code))

st1 = status()
check("no restart", st1["uptime"] > st0["uptime"] and st1["reset_cause"] == st0["reset_cause"],
      f"uptime {st0['uptime']} -> {st1['uptime']}")
latched = [f for f in st1["faults"] if f.startswith("sensor_") and f not in st0["faults"]]
check("no sensor fault was latched (SNS-PRES-17)", not latched,
      f"{','.join(latched) or 'none'}; readings beside a flash operation +{st1['pres_flashed'] - st0['pres_flashed']}")
print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
