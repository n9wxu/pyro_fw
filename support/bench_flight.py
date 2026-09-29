#!/usr/bin/env python3
"""Fly a profile on a board on the bench (src/bench_flight.h, DD-078).

    support/bench_flight.py 192.168.42.1                      3 km, the board's defaults
    support/bench_flight.py 192.168.42.1 --apogee 30000 --boost 4 --drogue 25
    support/bench_flight.py 192.168.42.1 --no-thin            a drogue at one rate

Turns test mode on, starts the profile, and follows /api/sim and /api/status
once a second until the machine lands and the profile ends. Prints each state
change, the fires, and what the loop, the flash and the logs counted. Every
fire on the board is mocked from the start until it reboots.

Exit status 1 when the flight did not land, the drogue did not fire within
APOGEE_TOL_S of the profile's apogee, the main did not fire, the loop overran
or a flash operation was refused.
"""
import argparse
import json
import sys
import time
import urllib.request

APOGEE_TOL_S = 4.0  # the drogue's fire, as seen at this script's 1 s poll


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return json.loads(r.read())


def post(host, path):
    req = urllib.request.Request(f"http://{host}{path}", data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return e.code, json.loads(body or b"{}")
        except ValueError:
            return e.code, {"body": body.decode(errors="replace")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    # None: the board's default, so the path stays inside HTTP_PATH_MAX (64).
    ap.add_argument("--apogee", type=float, help="metres above the pad (3000)")
    ap.add_argument("--boost", type=float, help="seconds (2)")
    ap.add_argument("--drogue", type=float, help="m/s in the pad's air (25)")
    ap.add_argument("--main-alt", type=float, help="metres above the pad (300)")
    ap.add_argument("--main", type=float, help="m/s (6)")
    ap.add_argument("--pad", type=float, help="seconds on the pad first (5)")
    ap.add_argument("--no-thin", action="store_true", help="the drogue's rate at every height")
    ap.add_argument("--timeout", type=float, default=3600.0)
    a = ap.parse_args()

    st = get(a.host, "/api/status")
    if st["state"] != "PAD_IDLE":
        print(f"the board is in {st['state']}; reboot it to fly again")
        return 1
    base = {k: st.get(k, 0) for k in ("loop_overruns", "flash_refusals", "flash_skips", "log_dropped")}
    code, r = post(a.host, "/api/test_mode/on")
    if code != 200 or not r.get("test_mode"):
        print("test mode refused:", code, r)
        return 1
    given = [("apogee", a.apogee), ("boost", a.boost), ("drogue", a.drogue), ("main_alt", a.main_alt),
             ("main", a.main), ("pad", a.pad), ("thin", 0 if a.no_thin else None)]
    q = "&".join(f"{k}={v:g}" for k, v in given if v is not None)
    path = "/api/sim/flight" + ("?" + q if q else "")
    if len(path) >= 64:
        print(f"{path} is {len(path)} characters; the board takes 63. Leave defaults out.")
        return 1
    code, r = post(a.host, path)
    if code != 200:
        print("start refused:", code, r)
        return 1
    print(f"flying {q}")

    t0 = time.monotonic()
    state = phase = None
    fires = [0, 0]
    fire_t = [None, None]
    peak_alt = 0.0
    hr0 = None
    try:
        hr0 = get(a.host, "/api/hr")
    except Exception:
        pass
    while time.monotonic() - t0 < a.timeout:
        try:
            s = get(a.host, "/api/sim")
            st = get(a.host, "/api/status")
        except Exception as e:  # a slow answer is not a failed flight
            print(f"{time.monotonic() - t0:7.1f}  no answer: {e}")
            time.sleep(1)
            continue
        el = time.monotonic() - t0
        peak_alt = max(peak_alt, st["max_alt_cm"] / 100.0)
        if st["state"] != state or s["phase"] != phase:
            print(f"{el:7.1f}  {st['state']:15s} profile {s['phase']:7s} t={s['t_s']:7.1f}s "
                  f"alt={s['alt_m']:8.1f} m  {s['pa']:9.1f} Pa  board alt={st['alt_cm'] / 100:8.1f} m "
                  f"v={st['vspeed_cms'] / 100:7.1f} m/s  lock={st['mach_lock']}")
            state, phase = st["state"], s["phase"]
        if s["fires"] != fires:
            for ch in (0, 1):
                if s["fires"][ch] != fires[ch]:
                    print(f"{el:7.1f}  PYRO{ch + 1} fired (mocked) at profile alt {s['alt_m']:.1f} m, "
                          f"t={s['t_s']:.1f}s")
                    if fire_t[ch] is None:
                        fire_t[ch] = s["t_s"]
            fires = list(s["fires"])
        if st["state"] == "LANDED" and not s["flying"]:
            break
        time.sleep(1)

    st = get(a.host, "/api/status")
    s = get(a.host, "/api/sim")
    print()
    print(f"profile peak {s['peak_m']:.0f} m; board max_alt {st['max_alt_cm'] / 100:.0f} m "
          f"(SNS-ALT-02 clamps at 8000); main_forced={st['main_forced']} refires={st['pyro1_refires']}")
    counts = {k: st.get(k, 0) - base[k] for k in base}
    print("during the flight:", ", ".join(f"{k} +{v}" for k, v in counts.items()),
          f"loop_max_us {st['loop_max_us']}, loop_late_max_us {st['loop_late_max_us']}")
    try:
        hr = get(a.host, "/api/hr")
        print(f"hr log: logs {hr['logs'] - (hr0 or hr)['logs']:+d}, file {hr['file']}, "
              f"bytes_total {hr['bytes_total']}, dropped {hr['dropped_records']}, "
              f"imu_overruns {hr['imu_overruns']}, write_max_us {hr['write_max_us']}, card {hr['card']}")
    except Exception:
        pass
    ok = st["state"] == "LANDED" and s["fires"][0] >= 1 and s["fires"][1] >= 1
    ok &= counts["loop_overruns"] == 0 and counts["flash_refusals"] == 0
    late = None if fire_t[0] is None else fire_t[0] - s["t_apogee_s"]
    print(f"drogue {'never fired' if late is None else f'{late:+.1f} s from the profile apogee'} "
          f"(t={s['t_apogee_s']:.1f} s)")
    ok &= late is not None and abs(late) <= APOGEE_TOL_S
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
