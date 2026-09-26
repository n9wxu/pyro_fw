#!/usr/bin/env python3
"""
api_check.py -- bench verification of the HTTP API against a live board.

Exercises what a board sitting on a bench can show: every route answers with
its status line, CORS and a framed body; a disabled channel survives the
config merge (CFG-04) and stops being a pad fault (FLT-BOOT-16); the pad
diagnosis and the verdict it would announce follow the config without a
power cycle (PYR-CONT-03); a board reached this way is on USB, so it reports
usb_attached and its buzzer is silent (USB-01..03), and test mode gives it its
voice and its pad marker back until it is turned off (USB-08); no inert key is written (CFG-SUBSYS-01); the flight log can be
erased (WEB-API-09); /api/status carries the refusal and emergency fields.

ERASES THE FLIGHT LOG, changes pyro1's mode for a few seconds, and holds the
board in test mode for about 15 s -- long enough to write the pad marker. A
launch is possible in that time if the barometer sees one. The board's
pyro settings are restored before exit. Refuse to run on a board that is not
in PAD_IDLE.

Usage:
    ./support/api_check.py 192.168.7.1

SPDX-License-Identifier: MIT
"""
import json
import sys
import time
import urllib.request
import urllib.error

HOST = sys.argv[1]
BASE = f"http://{HOST}"
results = []


def req(method, path, body=None, timeout=10):
    data = body.encode() if isinstance(body, str) else body
    r = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        r.add_header("Content-Type", "text/plain")
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail else ""))


def status():
    s, _, b = req("GET", "/api/status")
    return json.loads(b)


def wait_for(pred, secs=6):
    end = time.time() + secs
    while time.time() < end:
        st = status()
        if pred(st):
            return st
        time.sleep(0.5)
    return status()


st0 = status()
if st0["state"] != "PAD_IDLE":
    sys.exit(f"{HOST} is in {st0['state']}; run this on the pad only")
board = st0["board"]
print(f"== {board} {st0['fw_version']} at {HOST}, state {st0['state']}")
NEW_FIELDS = ("pyro1_refused", "pyro2_refused", "pyro1_refires", "main_forced", "usb_attached", "test_mode",
              "buzzer_active")
check("status: new fields present", all(k in st0 for k in NEW_FIELDS),
      ",".join(k for k in NEW_FIELDS if k not in st0))
check("status: PAD_IDLE flight time is 0", st0["state"] != "PAD_IDLE" or st0["flight_ms"] == 0, str(st0["flight_ms"]))
# USB is the only way to reach the API, so this board has a host on its port.
check("status: usb_attached is true", st0.get("usb_attached") is True, str(st0.get("usb_attached")))
if st0["uptime"] > 5000:
    check("status: a board on USB is silent", st0.get("buzzer_active") is False, str(st0.get("buzzer_active")))

# REV-17 / REV-23: every one-shot response is framed and carries CORS.
for method, path, want in (("GET", "/www/no_such_file.html", 404), ("POST", "/api/no_such_route", 404),
                           ("GET", "/api/config", 200), ("GET", "/api/pins", 200),
                           ("GET", "/api/pins/caps", 200), ("GET", "/api/beeps", 200),
                           ("GET", "/api/flight.csv", 200)):
    code, hdr, body = req(method, path, "" if method == "POST" else None)
    cors = hdr.get("Access-Control-Allow-Origin") == "*"
    check(f"{method} {path} -> {want} with CORS", code == want and cors, f"{code} cors={cors}")
code, hdr, body = req("GET", "/www/no_such_file.html")
check("404 body is framed by Content-Length", hdr.get("Content-Length") == str(len(body)),
      f"len={hdr.get('Content-Length')} body={len(body)}")

# REV-10: the erase endpoint, and the empty log reads as the column header.
code, _, body = req("POST", "/api/flight/erase")
check("POST /api/flight/erase -> 200", code == 200 and b"erased" in body, f"{code} {body[:60]!r}")
code, _, body = req("GET", "/api/flight.csv")
check("erased log reads as the bare column header", body.strip() == b"time_ms,pressure_pa,altitude_cm,state,thrust,event",
      repr(body[:80]))

# REV-02 / REV-12: a disabled channel survives the merge, and no inert key is written.
_, _, cfg0 = req("GET", "/api/config")
cfg0 = cfg0.decode()
kv0 = dict(l.split("=", 1) for l in cfg0.replace("\r", "").split("\n") if "=" in l)
orig = {k: kv0[k] for k in ("pyro1_mode", "pyro1_value", "pyro2_mode", "pyro2_value") if k in kv0}
print(f"   original pyro config: {orig}")

code, _, body = req("POST", "/api/config", "[pyro]\r\npyro1_mode=none\r\npyro1_value=0\r\n")
check("POST /api/config pyro1_mode=none applied", code == 200 and b"applied" in body, f"{code} {body[:60]!r}")
_, _, cfg1 = req("GET", "/api/config")
cfg1 = cfg1.decode()
check("config.ini keeps pyro1_mode=none", "pyro1_mode=none" in cfg1)
inert = [k for k in ("beep_mode=", "max_coast_s=", "log_enabled=", "buzzer_startup=") if k in cfg1]
check("config.ini carries no inert keys after a save", not inert, ",".join(inert))
st1 = wait_for(lambda s: s["pyro1_mode"] == "none" and "pyro1_open" not in s["faults"])
check("status reports pyro1_mode none", st1["pyro1_mode"] == "none")

# REV-04 + disabled channel: the pad diagnosis follows the config within a check or two.
if not st0["pyro1_cont"] and "pyro1_open" in st0["faults"]:
    check("disabled channel 1 no longer reported open", "pyro1_open" not in st1["faults"], str(st1["faults"]))
    want = "check_pyro_2" if "pyro2_open" in st1["faults"] else "ok_to_fly"
    check(f"verdict moved off check_pyro_1 without a power cycle", st1["beep"] == want,
          f"beep={st1['beep']} faults={st1['faults']}")

# Restore.
restore = "[pyro]\r\n" + "".join(f"{k}={v}\r\n" for k, v in orig.items())
code, _, body = req("POST", "/api/config", restore)
st2 = wait_for(lambda s: s["pyro1_mode"] == orig.get("pyro1_mode", s["pyro1_mode"]) and
               (st0["faults"] == [] or s["faults"] == st0["faults"]))
check("config restored", code == 200 and st2["pyro1_mode"] == orig.get("pyro1_mode"), f"{st2['pyro1_mode']}")
check("diagnosis restored with it", st2["faults"] == st0["faults"] and st2["beep"] == st0["beep"],
      f"faults={st2['faults']} beep={st2['beep']}")

check("still silent after the config changes", status().get("buzzer_active") is False)

# USB-08: test mode. Off unless someone turned it on this boot.
check("test mode is off", st0.get("test_mode") is False, str(st0.get("test_mode")))
code, _, body = req("POST", "/api/test_mode/maybe")
check("POST /api/test_mode/<other> -> 404", code == 404, f"{code} {body[:40]!r}")
programs0 = status()["flash_programs"]
try:
    code, _, body = req("POST", "/api/test_mode/on")
    check("POST /api/test_mode/on -> 200", code == 200 and b'"test_mode":true' in body, f"{code} {body[:40]!r}")
    st3 = wait_for(lambda s: s["buzzer_active"], secs=4)
    check("test mode: the pad verdict is announced on USB", st3["test_mode"] and st3["buzzer_active"],
          f"test_mode={st3['test_mode']} buzzer_active={st3['buzzer_active']}")
    st3 = wait_for(lambda s: s["flash_programs"] > programs0, secs=14)
    check("test mode: the pad marker is written on USB", st3["flash_programs"] > programs0,
          f"flash_programs {programs0} -> {st3['flash_programs']}")
    code, _, mk = req("GET", "/pad.mkr")
    check("pad.mkr holds a marker", code == 200 and len(mk) == 16, f"{code} {len(mk)} B")
finally:
    code, _, body = req("POST", "/api/test_mode/off")
check("POST /api/test_mode/off -> 200", code == 200 and b'"test_mode":false' in body, f"{code} {body[:40]!r}")
st4 = wait_for(lambda s: not s["buzzer_active"], secs=4)
check("out of test mode, one chirp and then silence", not st4["buzzer_active"] and not st4["test_mode"],
      f"test_mode={st4['test_mode']} buzzer_active={st4['buzzer_active']}")
check("no flash refused throughout", st4["flash_refusals"] == 0, str(st4["flash_refusals"]))

failed = [r for r in results if not r[1]]
print(f"== {len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
