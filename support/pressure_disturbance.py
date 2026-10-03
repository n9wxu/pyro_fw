#!/usr/bin/env python3
"""
pressure_disturbance.py -- does a board's own buzzer or flash disturb its
pressure sensor? (DD-068, G4-M, docs/pressure_collector.md)

    support/pressure_disturbance.py HOST [SAVE.json]     measure one board
    support/pressure_disturbance.py --flash-first HOST [SAVE.json]
    support/pressure_disturbance.py --analyze SAVE.json...

Takes every conversion from /api/pressure/trace through four phases: 20 s at
rest, four beep codes (POST /api/beeps/play, which writes no flash), 10 s at
rest, and 25 s of config saves (each rewrites config.ini as it stands, so it
writes flash and changes nothing). The board must be on the pad.

Reports, per phase, the pressure's departures from a 9-point running median
(slow drift does not count): the typical one, their rms, which a burst
raises, and the worst; the raw codes' rms; and how far the conversions the
firmware marked as flashed sat from their neighbours. It also says so if the sensor stopped answering, and when.
"""
import json
import math
import statistics
import sys
import threading
import time
import urllib.request

import pressure_trace as pt

REST_S, BEEPS, BEEP_S, REST2_S, FLASH_S = 20, 4, 5.6, 10, 25


def measure(host, save, flash_first=False):
    def get(path):
        with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
            return r.read()

    def post(path, body, ctype):
        req = urllib.request.Request(f"http://{host}{path}", data=body, method="POST",
                                     headers={"X-Pyro": "1", "Content-Type": ctype})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code
        except OSError as e:
            return str(e)

    def counters():
        d = json.loads(get("/api/status"))
        return {k: d.get(k, 0) for k in ("board", "sensor", "state", "flash_programs", "flash_erases", "pres_flashed",
                                         "pres_rejects", "pres_recoveries", "pres_dropped", "pres_bus")}

    before = counters()
    if before["state"] != "PAD_IDLE":
        sys.exit(f"{host} is in {before['state']}, not on the pad")
    config = get("/api/config")
    phase, recs, stop = ["settle"], [], []

    def collector():
        _, since, _ = pt.fetch(host, 0xFFFFFFFF)
        while not stop:
            try:
                _, nxt, got = pt.fetch(host, since)
            except Exception:
                time.sleep(0.2)
                continue
            recs.extend((phase[0],) + r for r in got)
            since = nxt
            time.sleep(0.08)

    t = threading.Thread(target=collector)
    t.start()
    beeps, saves = [], []

    def rest(name, seconds):
        phase[0] = name
        time.sleep(seconds)

    def beep():
        phase[0] = "beep"
        for _ in range(BEEPS):
            beeps.append(post("/api/beeps/play", b'{"kind":"code","d1":9,"d2":9}', "application/json"))
            time.sleep(BEEP_S)

    def flash():
        phase[0] = "flash"
        end = time.time() + FLASH_S
        while time.time() < end:
            saves.append(post("/api/config", config, "text/plain"))
            time.sleep(0.4)

    rest("settle", 3)
    rest("rest", REST_S)
    (flash if flash_first else beep)()
    rest("settle", 3)
    rest("rest2", REST2_S)
    (beep if flash_first else flash)()
    rest("settle", 2)
    stop.append(True)
    t.join()
    after = counters()
    d = {"host": host, "before": before, "after": after, "recs": recs, "beeps": beeps, "saves": saves}
    if save:
        with open(save, "w") as f:
            json.dump(d, f)
    return d


def residuals(xs):
    return [xs[i] - statistics.median(xs[i - 4:i + 5]) for i in range(4, len(xs) - 4)]


def scatter(xs):
    """The typical departure (robust: bursts do not move it), the rms (bursts do), and the worst."""
    if len(xs) < 20:
        return float("nan"), float("nan"), float("nan")
    res = residuals(xs)
    mad = statistics.median(abs(r) for r in res)
    rms = math.sqrt(sum(r * r for r in res) / len(res))
    return 1.4826 * mad * 1.08, rms * 1.08, max(abs(r) for r in res)  # a 9-point median takes a little of the noise


def report(d):
    b, a = d["before"], d["after"]
    print(f"== {b['board']} ({b['sensor']}) {d['host']}: {len(d['beeps'])} beep codes {sorted(set(map(str, d['beeps'])))}, "
          f"{len(d['saves'])} config saves, {a['flash_programs'] - b['flash_programs']} flash programs, "
          f"{a['flash_erases'] - b['flash_erases']} erases, {a['pres_flashed'] - b['pres_flashed']} readings marked flashed")
    failed = a["pres_rejects"] - b["pres_rejects"]
    if failed:
        print(f"   THE SENSOR STOPPED ANSWERING: {failed} failed transfers {a['pres_bus']}, "
              f"{a['pres_recoveries'] - b['pres_recoveries']} recoveries")
    rows = [tuple(r) for r in d["recs"]]
    bmp = b["sensor"] == "BMP280"
    seen = []
    for r in rows:
        if r[0] != "settle" and r[0] not in seen:
            seen.append(r[0])
    for ph in seen:
        pressures = [r for r in rows if r[0] == ph and chr(r[6]) in "PF"]
        temps = [float(r[4]) for r in pressures] if bmp else [float(r[3]) for r in rows if r[0] == ph and chr(r[6]) in "TG"]
        typical, rms, worst = scatter([r[5] / 100.0 for r in pressures])
        line = (f"   {ph:6s} {len(pressures):5d} pressures: typical {typical:5.2f} Pa, rms {rms:5.2f} Pa, worst {worst:5.1f} Pa; "
                f"raw codes rms: pressure {scatter([float(r[3]) for r in pressures])[1]:6.1f}, temperature {scatter(temps)[1]:6.1f}")
        marked = [i for i, r in enumerate(pressures) if chr(r[6]) == "F"]
        off = []
        for i in marked:
            near = [pressures[j][5] / 100.0 for j in range(max(0, i - 6), min(len(pressures), i + 7))
                    if chr(pressures[j][6]) == "P"]
            if len(near) >= 4:
                off.append(pressures[i][5] / 100.0 - statistics.median(near))
        if off:
            line += (f"; {len(marked)} marked flashed, {math.sqrt(sum(o * o for o in off) / len(off)):.1f} Pa rms "
                     f"from their neighbours, worst {max(abs(o) for o in off):.1f}")
        print(line)
    if rows:
        last_phase = rows[-1][0]
        if last_phase != "settle":
            first = next(r for r in rows if r[0] == last_phase)
            print(f"   the last conversion came {((rows[-1][1] - first[1]) & 0xFFFFFFFF) / 1e6:.1f} s into '{last_phase}'")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--analyze":
        for path in sys.argv[2:]:
            with open(path) as f:
                report(json.load(f))
        return
    args = [a for a in sys.argv[1:] if a != "--flash-first"]
    if not args:
        sys.exit(__doc__)
    report(measure(args[0], args[1] if len(args) > 1 else None, flash_first="--flash-first" in sys.argv))


if __name__ == "__main__":
    main()
