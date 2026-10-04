#!/usr/bin/env python3
"""
api_check.py -- bench verification of the HTTP API against a live board.

Exercises what a board sitting on a bench can show: every route answers with
its status line, CORS and a framed body; a disabled channel survives the
config merge (CFG-04); a saved configuration is stored and the running board
keeps the one it started with (CFG-10); the board serves its pyro limits and
its sensor's range (PYR-BOARD-03, SNS-MAX-01); a board reached this way is on
USB, so it reports usb_attached and its buzzer is silent (USB-01..03), and
test mode gives it its voice and its pad record back until it is turned off
(USB-08); no inert key is written; the flight log can be erased (WEB-API-09);
/api/status carries the pulse, fault, resume and emergency fields.

Verifies on hardware [WEB-API-01, WEB-API-02, WEB-API-03, WEB-API-07,
WEB-API-12, BUZ-CODE-03, BUZ-CODE-11, FLT-BROWN-01, FLT-BROWN-05].

ERASES THE FLIGHT LOG, stores a changed pyro1 mode for a few seconds, and
holds the board in test mode for about 15 s -- long enough to write the pad
record. A launch is possible in that time if the barometer sees one. The
stored pyro settings are restored before exit. Refuses to run on a board that
is not in PAD_IDLE.

Usage:
    ./support/api_check.py 192.168.7.1

SPDX-License-Identifier: MIT
"""
import json
import struct
import sys
import time
import urllib.request
import urllib.error

HOST = sys.argv[1]
BASE = f"http://{HOST}"
results = []


def marker_valid(mk):
    """src/flight_resume.c's pad_record_valid(): pad_record_t, five
    little-endian words, version 3 (DD-086)."""
    if len(mk) != 20:
        return False
    magic, version, ground, sigma, total = struct.unpack("<IIiII", mk)
    want = (magic ^ ((version * 2654435761) & 0xFFFFFFFF) ^ (ground & 0xFFFFFFFF) ^
            ((sigma * 40503) & 0xFFFFFFFF))
    return (magic == 0x50594D31 and version == 3 and 50000 <= ground <= 110000
            and 0 < sigma <= 100000 and total == want)


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
NEW_FIELDS = ("resume", "pyro_pulses", "pyro_fault", "emergency_fire", "refire_interval_ms", "fire_gap_ms",
              "pyro_limited", "usb_attached", "test_mode",
              "buzzer_active", "raw_pa", "pad_speed_cms", "ground_degraded", "ground_reseeds",
              "sample_interval_us", "stamp_lag_max_us", "noise_mpa", "estimator", "estimator_explains",
              "peak_lower_bound", "log_rate")
check("status: new fields present", all(k in st0 for k in NEW_FIELDS),
      ",".join(k for k in NEW_FIELDS if k not in st0))
# T0: the sensor's own reading, and the speed the launch detector reads.
check("status: raw_pa is a pressure", 30000 <= st0.get("raw_pa", 0) <= 110000, str(st0.get("raw_pa")))
check("status: pad_speed_cms is a pad's speed", abs(st0.get("pad_speed_cms", 99999)) < 2000,
      str(st0.get("pad_speed_cms")))
# SNS-EST-01: the sensor noise the estimator tracks, from the quietest part's floor up. A board on USB alone
# reads 6 to 10 Pa where it reads 2 to 2.6 on a battery (docs/pressure_collector.md).
check("status: noise_mpa is the pad's measured noise", 1200 <= st0.get("noise_mpa", 0) <= 20000,
      str(st0.get("noise_mpa")))
# FLT-BROWN-05: a board on USB says why this start did not resume a flight.
check("status: resume says why this start is not a flight", str(st0.get("resume", "")).startswith("not resumed"),
      str(st0.get("resume")))
check("status: nothing has been pulsed on the pad", st0.get("pyro_pulses") == [0, 0] and not st0.get("emergency_fire"),
      f'{st0.get("pyro_pulses")} {st0.get("emergency_fire")}')

# PYR-BOARD-03, SNS-MAX-01: the board's own limits, and what is in force is inside them.
code, _, body = req("GET", "/api/limits")
lim = json.loads(body) if code == 200 else {}
for key in ("refire_interval_ms", "fire_gap_ms"):
    r = lim.get(key, {})
    ok = code == 200 and r.get("min", 1) <= r.get("default", 0) <= r.get("max", 0) and \
        r.get("min", 1) <= r.get("in_force", 0) <= r.get("max", 0) and st0.get(key) == r.get("in_force")
    check(f"limits: {key} default and in force are inside the board's range", ok, str(r))
check("limits: the sensor's range and height", lim.get("sensor", {}).get("height_m", 0) >= 9000 and
      0 < lim["sensor"]["min_pa"] < lim["sensor"]["max_pa"], str(lim.get("sensor")))
# SNS-EST-06: the estimators the build carries, and the one obeyed is among them.
estimators = lim.get("estimators", [])
check("limits: the estimators carried, and status names the one obeyed", len(estimators) >= 1 and
      st0.get("estimator") in estimators, f'{estimators} obeying {st0.get("estimator")}')
check("status: PAD_IDLE flight time is 0", st0["state"] != "PAD_IDLE" or st0["flight_ms"] == 0, str(st0["flight_ms"]))
# USB is the only way to reach the API, so this board has a host on its port.
check("status: usb_attached is true", st0.get("usb_attached") is True, str(st0.get("usb_attached")))
if st0["uptime"] > 5000:
    check("status: a board on USB is silent", st0.get("buzzer_active") is False, str(st0.get("buzzer_active")))

# Every one-shot response is framed and carries CORS.
for method, path, want in (("GET", "/www/no_such_file.html", 404), ("POST", "/api/no_such_route", 404),
                           ("GET", "/api/config", 200), ("GET", "/api/pins", 200),
                           ("GET", "/api/pins/caps", 200), ("GET", "/api/beeps", 200),
                           ("GET", "/api/flight.csv", 200), ("GET", "/api/log/space", 200),
                           ("GET", "/api/net", 200), ("GET", "/api/limits", 200)):
    code, hdr, body = req(method, path, "" if method == "POST" else None)
    cors = hdr.get("Access-Control-Allow-Origin") == "*"
    check(f"{method} {path} -> {want} with CORS", code == want and cors, f"{code} cors={cors}")
code, hdr, body = req("GET", "/www/no_such_file.html")
check("404 body is framed by Content-Length", hdr.get("Content-Length") == str(len(body)),
      f"len={hdr.get('Content-Length')} body={len(body)}")

# The erase endpoint, and the empty log reads as the column header.
code, _, body = req("POST", "/api/flight/erase")
check("POST /api/flight/erase -> 200", code == 200 and b"erased" in body, f"{code} {body[:60]!r}")
code, _, body = req("GET", "/api/flight.csv")
check("erased log reads as the bare column header", body.strip() == b"time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event",
      repr(body[:80]))

# DD-062: the room the next flight's log has, in bytes and binary records.
_, _, body = req("GET", "/api/log/space")
sp = json.loads(body)
check("log space: bytes free, record size and the two rates",
      sp.get("bytes_free", 0) > 0 and sp.get("record_bytes") == 22 and sp.get("rates_hz", [0])[0] == 1
      and 40 <= sp.get("rates_hz", [0, 0])[1] <= 100, str(sp))

# GND-TEST-12: the ground test switch's wiring and pads, as pins.ini has them.
_, _, body = req("GET", "/api/pins/caps")
pc = json.loads(body)
check("pins caps: the ground test switch", pc.get("ground_test") in ("none", "ground", "pair")
      and isinstance(pc.get("gt_pin"), int) and isinstance(pc.get("gt_drive_pin"), int),
      f'{pc.get("ground_test")} {pc.get("gt_pin")} {pc.get("gt_drive_pin")}')

# G4-N: the network's counters. This request's own connection is open, so
# accepts and an established connection are at least one.
_, _, body = req("GET", "/api/net")
nt = json.loads(body)
check("net: lwIP's pools, TCP by state, the transport's refusals",
      all(len(nt.get(k, [])) == 3 for k in ("heap", "tcp_pcb", "tcp_seg", "pbuf_pool"))
      and len(nt.get("states", [])) == 11 and nt["states"][4] >= 1 and nt.get("accepts", 0) >= 1
      and len(nt.get("usb", [])) == 4 and nt["usb"][0] >= 1, str(nt)[:160])

# CFG-04, CFG-06, CFG-10: a save is merged into the stored file, writes no
# inert key, and changes nothing on the running board.
_, _, cfg0 = req("GET", "/api/config")
cfg0 = cfg0.decode()
kv0 = dict(l.split("=", 1) for l in cfg0.replace("\r", "").split("\n") if "=" in l)
orig = {k: kv0[k] for k in ("pyro1_mode", "pyro1_value", "pyro2_mode", "pyro2_value", "estimator") if k in kv0}
print(f"   stored pyro config: {orig}")

other = next((e for e in estimators if e != st0.get("estimator")), st0.get("estimator"))
code, _, body = req("POST", "/api/config", f"[pyro]\r\npyro1_mode=none\r\npyro1_value=0\r\nestimator={other}\r\n")
check("POST /api/config is stored and asks for a reboot", code == 200 and b'"reboot_required":true' in body,
      f"{code} {body[:80]!r}")
_, _, cfg1 = req("GET", "/api/config")
cfg1 = cfg1.decode()
check("config.ini keeps pyro1_mode=none", "pyro1_mode=none" in cfg1)
check("config.ini keeps the estimator chosen (SNS-EST-06)", f"estimator={other}" in cfg1, other)
check("the merge kept what the post left out", f"pyro2_mode={orig.get('pyro2_mode')}" in cfg1)
inert = [k for k in ("beep_mode=", "max_coast_s=", "log_enabled=", "buzzer_startup=", "log_rate_hz=",
                     "telem_format=", "telem_rate_hz=") if k in cfg1]
check("config.ini carries no inert keys after a save", not inert, ",".join(inert))
time.sleep(2.5)
st1 = status()
check("the running board keeps the configuration it started with",
      st1["pyro1_mode"] == st0["pyro1_mode"] and st1["faults"] == st0["faults"] and st1["beep"] == st0["beep"] and
      st1["estimator"] == st0["estimator"],
      f"pyro1_mode={st1['pyro1_mode']} faults={st1['faults']} beep={st1['beep']}")

# Restore the stored file.
orig.setdefault("estimator", st0.get("estimator"))
restore = "[pyro]\r\n" + "".join(f"{k}={v}\r\n" for k, v in orig.items())
code, _, body = req("POST", "/api/config", "[pyro]\r\npyro1_mode=delay\r\npyro2_value=70000\r\n")
_, _, cfg_refused = req("GET", "/api/config")
check("a value beyond its field is refused with 400 and nothing is stored (SYS-CFG-03)",
      code == 400 and cfg_refused.decode() == cfg1, f"{code} {body[:80]!r}")

code, _, body = req("POST", "/api/config", restore)
_, _, cfg2 = req("GET", "/api/config")
check("stored config restored", code == 200 and f"pyro1_mode={orig.get('pyro1_mode')}" in cfg2.decode() and
      f"estimator={orig.get('estimator')}" in cfg2.decode())

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
    check("test mode: the pad record is written on USB", st3["flash_programs"] > programs0,
          f"flash_programs {programs0} -> {st3['flash_programs']}")
    code, _, mk = req("GET", "/pad.mkr")
    check("pad.mkr holds a record", code == 200 and marker_valid(mk), f"{code} {len(mk)} B")
finally:
    code, _, body = req("POST", "/api/test_mode/off")
check("POST /api/test_mode/off -> 200", code == 200 and b'"test_mode":false' in body, f"{code} {body[:40]!r}")
st4 = wait_for(lambda s: not s["buzzer_active"], secs=4)
check("out of test mode, one chirp and then silence", not st4["buzzer_active"] and not st4["test_mode"],
      f"test_mode={st4['test_mode']} buzzer_active={st4['buzzer_active']}")
check("no flash refused throughout", st4["flash_refusals"] == 0, str(st4["flash_refusals"]))
# SNS-COL-04: the sensor answered every transfer, through the beeps and the flash writes above.
check("no sensor transfer failed throughout", st4["pres_rejects"] == st0["pres_rejects"],
      f'{st4["pres_rejects"] - st0["pres_rejects"]} failed: {st4.get("pres_bus")}, {st4.get("pres_recoveries")} recoveries')
sensor_faults = [f for f in st4.get("faults", []) if f.startswith("sensor_")]
check("no sensor fault was latched throughout (SNS-PRES-17)", not sensor_faults, ",".join(sensor_faults))

failed = [r for r in results if not r[1]]
print(f"== {len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
