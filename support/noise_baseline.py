#!/usr/bin/env python3
"""Measure a board's pressure noise on the bench.

    support/noise_baseline.py <board-ip> [seconds]

Polls /api/status for `seconds` (default 60) and reports the RMS of raw_pa
about a straight-line fit, so weather drift over the minute is not counted as
noise, and the RMS of pad_speed_cms, the speed the launch detector reads. The
host tests assume the MS5607's datasheet figure of 1.2 Pa (OSR 4096); this is
how that assumption is checked against each board's part.

The board must be still, in PAD_IDLE, with no one touching it.
"""
import json
import math
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "pyro.local"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0


def status():
    with urllib.request.urlopen(f"http://{HOST}/api/status", timeout=3) as r:
        return json.loads(r.read())


def rms_about_line(ts, ys):
    n = len(ts)
    mt, my = sum(ts) / n, sum(ys) / n
    stt = sum((t - mt) ** 2 for t in ts)
    slope = sum((t - mt) * (y - my) for t, y in zip(ts, ys)) / stt if stt else 0.0
    res = [y - (my + slope * (t - mt)) for t, y in zip(ts, ys)]
    return math.sqrt(sum(r * r for r in res) / n), slope


st = status()
missing = [k for k in ("raw_pa", "pad_speed_cms") if k not in st]
if missing:
    sys.exit(f"{HOST} ({st.get('board')} {st.get('fw_version')}) has no {', '.join(missing)} on /api/status: "
             "flash firmware with T0's status fields first")
if st["state"] != "PAD_IDLE":
    sys.exit(f"{HOST} is in {st['state']}; measure on the pad only")

ts, raws, speeds = [], [], []
t0 = time.time()
while time.time() - t0 < SECONDS:
    s = status()
    ts.append(time.time() - t0)
    raws.append(s["raw_pa"])
    speeds.append(s["pad_speed_cms"] / 100.0)
    time.sleep(0.05)

raw_rms, drift = rms_about_line(ts, raws)
speed_rms = math.sqrt(sum(v * v for v in speeds) / len(speeds))
print(f"{st['board']} {st['fw_version']} ({st['sensor']}) at {HOST}: {len(raws)} readings over {SECONDS:.0f} s")
print(f"  raw_pa: {raw_rms:.2f} Pa RMS about the trend (drift {drift * 60:+.1f} Pa/min), "
      f"mean {sum(raws) / len(raws):.0f} Pa")
print(f"  pad_speed: {speed_rms:.2f} m/s RMS")
