#!/usr/bin/env python3
"""Fly a profile on a board on the bench (src/bench_flight.h, DD-078).

Verifies on hardware [SYS-SIM-01, SIM-01..04, FLT-BROWN-04, FLT-LOG-07, SNS-PRES-14].

    support/bench_flight.py 192.168.42.1                      3 km, the board's defaults
    support/bench_flight.py 192.168.42.1 --apogee 30000 --boost 4 --drogue 25
    support/bench_flight.py 192.168.42.1 --no-thin            a drogue at one rate
    support/bench_flight.py 192.168.42.1 --fail drogue        its canopy never opens (SIM-04)
    support/bench_flight.py 192.168.42.1 --host-log f.bin     the log to the host, not the flash (USB-09)

Turns test mode on, starts the profile, and follows /api/sim and /api/status
once a second until the machine lands and the profile ends. Prints each state
change, the fires, and what the loop, the flash and the logs counted. Every
fire on the board is mocked from the start until it reboots.

Exit status 1 when the flight did not land, pyro 1 did not fire within
APOGEE_TOL_S of the profile's apogee, pyro 2 did not fire, the loop overran,
a flash operation was refused, or the log is missing a reading: a step
between sample rows from launch on wider than GAP_OF_MEDIAN times the median
step. A reading a flash operation ran beside is one of them. With --host-log,
also when a byte of the log was lost or the flash was written. With --fail, a board whose re-fire or
emergency speeds are set must also have fired a channel more than once.
"""
import argparse
import json
import statistics
import sys
import time
import urllib.request

APOGEE_TOL_S = 4.0  # the drogue's fire, as seen at this script's 1 s poll
GAP_OF_MEDIAN = 1.5  # one reading missing is twice the median step


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return json.loads(r.read())


def get_bytes(host, path, timeout=5):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return r.read()


def sample_times(d):
    """Each sample row's time, from the binary records (src/flight_log.h)."""
    i, times = 4, []
    if d[:4] != b"PYL1":
        return times
    while i < len(d):
        kind = d[i]
        if kind == ord("H"):
            i += 2 + d[i + 1]
        elif kind == ord("S") and i + 22 <= len(d):
            if d[i + 21] == 0:
                times.append(int.from_bytes(d[i + 1:i + 5], "little"))
            i += 22
        elif kind == ord("T") and i + 7 <= len(d):
            i += 7 + d[i + 6]
        else:
            break
    return times


def steps(times):
    """The widest step between sample rows from launch on, and the median.
    The rows kept from the pad all read 0."""
    t = [x for x in times if x > 0]
    iv = [b - a for a, b in zip(t, t[1:])]
    return (max(iv), statistics.median(iv)) if iv else (0, 0)


class HostLog:
    """The flight log as the board hands it to the host [USB-09, WEB-API-16]."""

    def __init__(self, host):
        self.host, self.data, self.lost, self.end = host, bytearray(), 0, 0

    def fetch(self):
        """Everything held since the last fetch; True once it has caught up."""
        while True:
            r = get_bytes(self.host, f"/api/flight/live?since={len(self.data) + self.lost}")
            first, self.end = int.from_bytes(r[0:4], "little"), int.from_bytes(r[4:8], "little")
            self.lost += max(0, first - (len(self.data) + self.lost))
            self.data += r[8:]
            if len(r) == 8 or len(self.data) + self.lost >= self.end:
                return

    def samples(self):
        return sample_times(self.data)


def get_text(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return r.read().decode(errors="replace")


def post(host, path):
    req = urllib.request.Request(f"http://{host}{path}", data=b"", method="POST",
                                 headers={"X-Pyro": "1"})  # the board refuses a POST without it
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
    ap.add_argument("--fail", choices=("drogue", "main", "both"), help="a canopy that never opens")
    ap.add_argument("--ballistic", type=float, help="m/s with no canopy out (80)")
    ap.add_argument("--timeout", type=float, default=3600.0)
    ap.add_argument("--host-log", metavar="FILE", help="keep the log off the flash and save it here (USB-09)")
    a = ap.parse_args()

    st = get(a.host, "/api/status")
    if st["state"] != "PAD_IDLE":
        print(f"the board is in {st['state']}; reboot it to fly again")
        return 1
    base = {k: st.get(k, 0) for k in ("loop_overruns", "flash_refusals", "flash_skips", "log_dropped",
                                      "flash_programs", "flash_erases", "pres_flashed")}
    code, r = post(a.host, "/api/test_mode/on" + ("?log=host" if a.host_log else ""))
    if code != 200 or not r.get("test_mode") or (a.host_log and r.get("test_log") != "host"):
        print("test mode refused:", code, r)
        return 1
    live = HostLog(a.host) if a.host_log else None
    given = [("apogee", a.apogee), ("boost", a.boost), ("drogue", a.drogue), ("main_alt", a.main_alt),
             ("main", a.main), ("pad", a.pad), ("thin", 0 if a.no_thin else None), ("ballistic", a.ballistic)]
    q = "&".join([f"{k}={v:g}" for k, v in given if v is not None] + ([f"fail={a.fail}"] if a.fail else []))
    path = "/api/sim/flight" + ("?" + q if q else "")
    if len(path) >= 64:
        print(f"{path} is {len(path)} characters; the board takes 63. Leave defaults out.")
        return 1
    code, r = post(a.host, path)
    if code != 200:
        print("start refused:", code, r)
        post(a.host, "/api/test_mode/off")  # as it was: a board that cannot fly is left on the pad
        return 1
    print(f"flying {q}")

    t0 = time.monotonic()
    state = phase = None
    fires = [0, 0]
    fire_t = [None, None]
    peak_alt = 0.0
    in_flight = []  # flash programs and erases at each poll from launch until landing
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
        if st["state"] not in ("PAD_IDLE", "LANDED"):
            in_flight.append(st["flash_programs"] + st["flash_erases"])
        if st["state"] != state or s["phase"] != phase:
            print(f"{el:7.1f}  {st['state']:15s} profile {s['phase']:7s} t={s['t_s']:7.1f}s "
                  f"alt={s['alt_m']:8.1f} m  {s['pa']:9.1f} Pa  board alt={st['alt_cm'] / 100:8.1f} m "
                  f"v={st['vspeed_cms'] / 100:7.1f} m/s  explains={st['estimator_explains']}")
            state, phase = st["state"], s["phase"]
        if s["fires"] != fires:
            for ch in (0, 1):
                if s["fires"][ch] != fires[ch]:
                    print(f"{el:7.1f}  PYRO{ch + 1} fired (mocked) at profile alt {s['alt_m']:.1f} m, "
                          f"t={s['t_s']:.1f}s")
                    if fire_t[ch] is None:
                        fire_t[ch] = s["t_s"]
            fires = list(s["fires"])
        if live:
            try:
                live.fetch()
            except Exception as e:
                print(f"{el:7.1f}  the log did not answer: {e}")
        if st["state"] == "LANDED" and not s["flying"]:
            break
        time.sleep(1)

    st = get(a.host, "/api/status")
    s = get(a.host, "/api/sim")
    print()
    print(f"profile peak {s['peak_m']:.0f} m; board max_alt {st['max_alt_cm'] / 100:.0f} m; "
          f"pulses {st['pyro_pulses']} emergency_fire={st['emergency_fire']} resume: {st['resume']}")
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
    time.sleep(2)  # the storage task hands the log's end over once LANDED has finalised
    if live:
        live.fetch()
        with open(a.host_log, "wb") as f:
            f.write(live.data)
        t = live.samples()
        written = in_flight[-1] - in_flight[0] if in_flight else 0
        print(f"host log: {len(live.data)} bytes to {a.host_log}, {live.lost} lost, "
              f"flash programs and erases from launch to landing {written}")
        ok &= live.lost == 0 and written == 0
    else:
        t = sample_times(get_bytes(a.host, "/flight_log.bin", timeout=60))
    widest, median = steps(t)
    print(f"log: {len(t)} sample rows, the widest step {widest} ms against a median {median:g} ms; "
          f"readings a flash operation ran beside +{counts['pres_flashed']}")
    ok &= len(t) > 0 and 0 < widest <= GAP_OF_MEDIAN * median
    late = None if fire_t[0] is None else fire_t[0] - s["t_apogee_s"]
    print(f"pyro 1 {'never fired' if late is None else f'{late:+.1f} s from the profile apogee'} "
          f"(t={s['t_apogee_s']:.1f} s)")
    ok &= late is not None and abs(late) <= APOGEE_TOL_S
    if a.fail:
        cfg = get_text(a.host, "/api/config")
        rules = [k for k in ("pyro1_refire_speed", "pyro2_refire_speed", "emergency_fire_speed")
                 if f"{k}=0" not in cfg.replace("\r", "").split("\n") and f"{k}=" in cfg]
        refired = max(st["pyro_pulses"]) > 1
        print(f"fire rules set: {', '.join(rules) or 'none'}; a channel fired again: {refired}")
        ok &= refired or not rules
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
