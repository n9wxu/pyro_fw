#!/usr/bin/env python3
"""
failed_write_check.py -- an upload the storage cannot finish, on a live board.

Uploads a file larger than the room the board reports, which the board
refuses part-way. Then, without a restart: the part file is gone, a delete of
the name finds no file, a small upload of the same name is taken once the
cleanup has erased a sector, and it reads back whole. The board's uptime runs
on throughout, and the sensor's readings beside a minute of flash writes keep
the pad's sensor check satisfied.

Verifies on hardware [WEB-API-18, SNS-PRES-14].

Leaves the board's storage holding the upload's pages until the pad's cleanup
erases them, a sector a second: about half an hour for 8 MB of files. A
flight before then erases in flight where its part can, and where it cannot
has only the room already erased (FLT-LOG-10). Refuses to run on a board
that is not in PAD_IDLE.

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


st0 = status()
if st0["state"] != "PAD_IDLE":
    sys.exit(f"{HOST} is in {st0['state']}; run this on the pad only")
print(f"== {st0['board']} {st0['fw_version']} at {HOST}")
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
code, waited = None, 0
for waited in range(30):  # the cleanup erases a sector a second
    code, _ = req("POST", NAME, small)
    if code == 201:
        break
    time.sleep(1)
check("a small upload of its name is taken", code == 201, f"{code} after {waited} s")
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
