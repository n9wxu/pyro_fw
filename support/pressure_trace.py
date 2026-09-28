#!/usr/bin/env python3
"""
pressure_trace.py -- every pressure conversion a board makes, checked for the
faults a sample rate hides.

    support/pressure_trace.py HOST [SECONDS [SAVE.json]]   capture and judge one board
    support/pressure_trace.py --analyze SAVE.json...       judge saved captures again
    support/pressure_trace.py --selftest       the judge, on made-up traces

Polls /api/pressure/trace (src/pressure_trace.h) and reports, per board:

  rate       pressure samples a second, and the spread of their intervals
  repeats    a raw pressure code equal to the one before it. Noise makes a
             few by chance, more where the codes step by more than one; the
             report sets the count against that chance. On the BMP280 a
             repeat of the temperature code too is a stale read: the same
             conversion taken twice, and on the BMP280 that is what is judged
  rejects    zeros (read before the conversion finished), bus errors, codes
             that compensate outside 1-120 kPa, and missed slots (the loop
             found the conversion still running)
  lag        from the driver's stamp to the loop reading it
  noise      the sample-to-sample scatter, in pascals

It is consistently good with no rejects, no missed slots, no stamp running
backwards, no gap the temperature conversion does not explain, no more repeats
than chance gives (a count chance reaches less than once in a thousand fails),
and no records lost to the ring (a lost record is the test's failure, not the
sensor's).
"""
import json
import math
import statistics
import struct
import sys
import time
import urllib.request

REC = struct.Struct("<IIIIiB3x")


def fetch(host, since):
    with urllib.request.urlopen(f"http://{host}/api/pressure/trace?since={since}", timeout=5) as r:
        b = r.read()
    if b[:4] != b"PTR1":
        raise ValueError(f"{host}: not a pressure trace")
    first, nxt = struct.unpack_from("<II", b, 4)
    recs = [REC.unpack_from(b, 12 + i * REC.size) for i in range((len(b) - 12) // REC.size)]
    return first, nxt, recs


def collect(host, seconds, poll=0.1):
    """Records for `seconds`, and the HTTP outages met on the way: a request
    that fails is retried, and how long the board stayed unreachable is kept."""
    first, nxt, _ = fetch(host, 0xFFFFFFFF)
    since, recs, lost, outages = nxt, [], 0, []
    down_at = None
    end = time.time() + seconds
    while time.time() < end:
        try:
            first, nxt, got = fetch(host, since)
        except OSError:
            down_at = down_at or time.time()
            time.sleep(0.5)
            continue
        if down_at:
            outages.append(time.time() - down_at)
            down_at = None
        lost += first - since
        recs += got
        since = nxt
        time.sleep(poll)
    if down_at:
        outages.append(time.time() - down_at)
    return recs, lost, outages


def s32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def robust_sigma(xs):
    if len(xs) < 3:
        return 0.0
    m = statistics.median(xs)
    return 1.4826 * statistics.median(abs(x - m) for x in xs)


def poisson_tail(k, lam):
    """P(X >= k) for X ~ Poisson(lam). Summed in logs: e**-lam underflows
    past a few hundred."""
    if k <= 0:
        return 1.0
    if lam > 50:
        return 0.5 * math.erfc((k - 0.5 - lam) / math.sqrt(2 * lam))
    total = sum(math.exp(i * math.log(lam) - lam - math.lgamma(i + 1)) for i in range(k))
    return max(0.0, 1.0 - total)


# More repeats than chance gives, when chance would give that many less often
# than this.
REPEAT_P = 1e-3


def analyze(recs, lost):
    kinds = {k: 0 for k in "PTZBRW"}
    for r in recs:
        kinds[chr(r[5])] = kinds.get(chr(r[5]), 0) + 1
    # One pass: the pressures, and whether a temperature conversion sat
    # between each and the one before, which explains one missing slot.
    p, t_between, seen_t = [], [], False
    for r in recs:
        k = chr(r[5])
        if k == "T":
            seen_t = True
        elif k == "P":
            if p:
                t_between.append(seen_t)
            p.append(r)
            seen_t = False
    out = {"lost": lost, "kinds": kinds, "n": len(p)}
    if len(p) < 20:
        out["verdict"] = "too few pressure samples"
        out["good"] = False
        return out
    bmp = any(r[3] for r in p)  # only the BMP280 reports a temperature code with each read
    iv = [s32((b[0] - a[0]) & 0xFFFFFFFF) for a, b in zip(p, p[1:])]
    span = sum(iv) / 1e6
    nominal = statistics.median(iv)
    backwards = sum(1 for d in iv if d <= 0)
    gaps = [d for d, t in zip(iv, t_between) if d > (2.5 if t else 1.5) * nominal]
    raw = [r[2] for r in p]
    d = [b - a for a, b in zip(raw, raw[1:])]
    # The codes' own step: an oversampling that leaves the low bits zero makes
    # every code a multiple, and a repeat that much likelier.
    step = 0
    for x in d:
        step = math.gcd(step, abs(x))
    step = step or 1
    sd = robust_sigma(d)
    p0 = min(1.0, step / (math.sqrt(2 * math.pi) * sd)) if sd > 0.5 else 1.0
    chance = p0 * len(d)
    repeats = sum(1 for x in d if x == 0)
    # Both codes repeated: chance is a pressure repeat among the pairs whose
    # temperature code happened to repeat.
    stale = sum(1 for a, b in zip(p, p[1:]) if a[2] == b[2] and a[3] == b[3]) if bmp else 0
    stale_chance = p0 * sum(1 for a, b in zip(p, p[1:]) if a[3] == b[3]) if bmp else 0.0
    lag = sorted(s32((r[1] - r[0]) & 0xFFFFFFFF) for r in p)
    pa = [r[4] / 100.0 for r in p]
    dpa = [b - a for a, b in zip(pa, pa[1:])]
    noise = robust_sigma(dpa) / math.sqrt(2)
    rejects = kinds["Z"] + kinds["B"] + kinds["R"]
    out.update(
        sensor="BMP280" if bmp else "MS5607",
        rate=len(p) / span if span > 0 else 0,
        nominal_ms=nominal / 1000,
        iv_ms=[min(iv) / 1000, statistics.median(iv) / 1000, max(iv) / 1000],
        backwards=backwards,
        gaps=len(gaps),
        gap_max_ms=max(gaps) / 1000 if gaps else 0,
        repeats=repeats,
        repeats_chance=chance,
        step=step,
        stale=stale,
        stale_chance=stale_chance,
        lag_ms=[lag[0] / 1000, lag[len(lag) // 2] / 1000, lag[-1] / 1000],
        noise_pa=noise,
        rejects=rejects,
    )
    faults = []
    if rejects:
        faults.append(f"{rejects} rejected conversions")
    if kinds["W"]:
        faults.append(f"{kinds['W']} missed slots")
    if backwards:
        faults.append(f"{backwards} stamps not after the one before")
    if gaps:
        faults.append(f"{len(gaps)} unexplained gaps, longest {max(gaps) / 1000:.1f} ms")
    if bmp and poisson_tail(stale, max(stale_chance, 0.1)) < REPEAT_P:
        faults.append(f"{stale} stale reads (both codes repeated) where chance gives {stale_chance:.1f}")
    # On the BMP280 a pressure code repeating alone is no evidence: at 50 Hz,
    # where a stale read cannot happen, they ran 19% over this estimate. A
    # stale read repeats both codes, and that is judged above.
    if not bmp and poisson_tail(repeats, max(chance, 0.1)) < REPEAT_P:
        faults.append(f"{repeats} repeated codes where chance gives {chance:.1f}")
    if lost:
        faults.append(f"{lost} records lost to the ring: poll faster (the test's fault)")
    out["good"] = not faults
    out["verdict"] = "consistently good" if not faults else "; ".join(faults)
    return out


def report(name, a):
    print(f"== {name}: {a['verdict']}")
    if "rate" not in a:
        return
    k = a["kinds"]
    print(f"   {a['sensor']}: {a['n']} pressures, {a['rate']:.1f}/s; intervals min/median/max "
          f"{a['iv_ms'][0]:.2f}/{a['iv_ms'][1]:.2f}/{a['iv_ms'][2]:.2f} ms; "
          f"temperatures {k['T']}; gaps {a['gaps']}")
    print(f"   code step {a['step']}; repeats {a['repeats']} (chance {a['repeats_chance']:.1f}), stale {a['stale']} "
          f"(chance {a['stale_chance']:.1f}); "
          f"zeros {k['Z']}, bus {k['B']}, range {k['R']}, missed {k['W']}, lost {a['lost']}")
    print(f"   lag min/median/max {a['lag_ms'][0]:.2f}/{a['lag_ms'][1]:.2f}/{a['lag_ms'][2]:.2f} ms; "
          f"noise {a['noise_pa']:.2f} Pa")


# ── Self-test ──────────────────────────────────────────────────────────


def synthetic(n=900, period_us=10000, t_every=10, noise_counts=80, bmp=False, stale_every=0, gap_at=None,
              zeros=0):
    import random
    rnd = random.Random(7)
    recs, t, raw = [], 1000, 6_500_000
    last = None
    for i in range(n):
        t += period_us
        if gap_at is not None and i == gap_at:
            t += 5 * period_us
        if not bmp and i % t_every == t_every - 1:
            recs.append((t & 0xFFFFFFFF, t + 3000, 8_000_000, 0, 0, ord("T")))
            continue
        if stale_every and last and i % stale_every == 0:
            recs.append(last[:0] + (t & 0xFFFFFFFF,) + last[1:])
            continue
        code = raw + int(rnd.gauss(0, noise_counts))
        pa = 101325 * 100 + int((code - raw) * 4.4)
        rec = (t & 0xFFFFFFFF, t + 3000, code, (500_000 + rnd.randint(0, 3)) if bmp else 0, pa, ord("P"))
        recs.append(rec)
        last = rec
    for i in range(zeros):
        recs.insert(100 + i, (recs[100][0], recs[100][1], 0, 0, 0, ord("Z")))
    return recs


def selftest():
    cases = [
        ("clean MS5607", synthetic(), True),
        ("clean BMP280", synthetic(bmp=True), True),
        ("stale BMP280 reads", synthetic(bmp=True, stale_every=6), False),
        ("stale MS5607 reads repeat the code", synthetic(stale_every=6), False),
        ("an unexplained gap", synthetic(gap_at=400), False),
        ("zeros", synthetic(zeros=3), False),
        ("a quiet sensor repeats by chance", synthetic(noise_counts=1), True),
    ]
    ok = True
    for name, recs, want in cases:
        a = analyze(recs, 0)
        good = a["good"] == want
        ok &= good
        print(f"{'PASS' if good else 'FAIL'} {name}: {a['verdict']}")
    a = analyze(synthetic(), 12)
    ok &= not a["good"]
    print(f"{'PASS' if not a['good'] else 'FAIL'} lost records fail the test: {a['verdict']}")
    big = poisson_tail(974, 805.8) < REPEAT_P and poisson_tail(820, 805.8) > REPEAT_P
    ok &= big
    print(f"{'PASS' if big else 'FAIL'} a large expected count is judged, not underflowed")
    return 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    if len(sys.argv) > 2 and sys.argv[1] == "--analyze":
        bad = 0
        for path in sys.argv[2:]:
            d = json.load(open(path))
            st = d["status"]
            a = analyze([tuple(r) for r in d["recs"]], d["lost"])
            report(f"{st['board']} {st['serial']} {st['fw_version']} ({st['sensor']}): {path}", a)
            bad += not a["good"]
        sys.exit(1 if bad else 0)
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    host = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    st = json.load(urllib.request.urlopen(f"http://{host}/api/status", timeout=5))
    recs, lost, outages = collect(host, seconds)
    if len(sys.argv) > 3:
        with open(sys.argv[3], "w") as f:
            json.dump({"status": st, "lost": lost, "outages": outages, "recs": recs}, f)
    a = analyze(recs, lost)
    report(f"{st['board']} {st['serial']} {st['fw_version']} ({st['sensor']}), {seconds:.0f} s", a)
    if outages:
        print(f"   HTTP outages: {len(outages)}, longest {max(outages):.1f} s (records lost meanwhile are counted above)")
    sys.exit(0 if a["good"] else 1)


if __name__ == "__main__":
    main()
