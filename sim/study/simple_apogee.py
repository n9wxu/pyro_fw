"""A simple apogee filter, and handing the decision to it when the model filter
does not converge.

    python3 sim/study/simple_apogee.py simple      simple flights, motor ejection spikes
    python3 sim/study/simple_apogee.py handover    every flight, port errors included
    python3 sim/study/simple_apogee.py chamber <flight.csv>

The simple filter is a running median of the raw readings. Apogee is the
median standing a set height above the lowest median since launch for a hold,
on a board that has been above the arming height. A new lowest median starts
the hold again.

A motor's ejection charge in the body tube is not announced to the board: its
spike arrives as readings. Its size and decay are not measured; the sweep
spans them.
"""
import math, os, random, sys
from collections import deque
from multiprocessing import Pool
from est_proto import Plant, port_error
from lumped_fly import ROCKETS, PORTS, COLD, HOT, DT
from lumped import Lumped
import apogee_fallback  # registers the 30 kft rocket

SCALE_H = 8000.0
ARM_H = 30.0
WINDOWS = (15, 25, 50)          # readings: 0.3, 0.5, 1 s
RISES = (2.0, 5.0, 10.0)        # m above the lowest median
HOLDS = (0.0, 0.2, 0.5)         # s
SIMPLE = ['hop', 'subsonic', 'mid-Mach']
EJECT_AT = (-3.0, -1.0, 0.0, 1.0, 3.0)      # s from the apogee of the same flight with no ejection
EJECT_PA = (3000.0, 30000.0, 100000.0)
EJECT_TAU = (0.02, 0.05, 0.2)
NOT_CONVERGED = (2.0, 4.0, 6.0, 8.0, 10.0)  # s of the model not explaining
NEVER_AFTER = (0.5, 1.0, 2.0)               # s after launch from which the model has never explained
SPEED_GATES = (150.0, 200.0, 250.0)         # m/s: the fastest climb the simple filter measured
SPEED_SPAN = 25                             # medians apart for its speed: 0.5 s


def simple_variants():
    return [(w, d, h) for w in WINDOWS for d in RISES for h in HOLDS]


class Simple:
    def __init__(self, window, rise, hold):
        self.raw = deque(maxlen=window)
        self.rise, self.hold = rise, hold
        self.ground = None
        self.lowest = None
        self.since = None
        self.high_enough = False
        self.medians = deque(maxlen=SPEED_SPAN + 1)
        self.max_climb = 0.0
        self.speed = None

    def step(self, p, t):
        """True while the rule is met."""
        if p is None:
            return self.since is not None and t - self.since >= self.hold
        self.raw.append(p)
        if len(self.raw) < self.raw.maxlen:
            return False
        m = sorted(self.raw)[len(self.raw) // 2]
        if self.ground is None:
            self.ground = m
        self.medians.append(m)
        if len(self.medians) > SPEED_SPAN:
            climb = (self.medians[0] - m) / m * SCALE_H / (SPEED_SPAN * DT)
            self.max_climb = max(self.max_climb, climb)
            self.speed = climb
        if m < self.ground * (1.0 - ARM_H / SCALE_H):
            self.high_enough = True
        if self.lowest is None or m < self.lowest:
            self.lowest, self.since = m, None
            return False
        if not self.high_enough or m <= self.lowest * (1.0 + self.rise / SCALE_H):
            self.since = None
            return False
        if self.since is None:
            self.since = t
        return t - self.since >= self.hold


def sensed_flight(rk, site, sigma, port, seed, eject=None, until=300.0):
    """Yields (flight time, reading or None, plant) from launch, 50 a second."""
    rng = random.Random(seed)
    pl = Plant(site, rk)
    tf = 0.0
    while not pl.landed and tf < until:
        for _ in range(20): pl.step(0.001)
        tf += DT
        ps = site.pressure(pl.h)
        sensed = ps + port_error(port, pl.mach, ps) + rng.gauss(0, sigma)
        if eject and tf >= eject[0]:
            sensed += eject[1] * math.exp(-(tf - eject[0]) / eject[2])
            if pl.canopy_ms is None: pl.canopy_ms = rk.canopy
        yield tf, (float(int(sensed)) if 1.0 <= sensed <= 130000.0 else None), pl


def apogee_time(rk, site):
    pl = Plant(site, rk)
    while not pl.apogee: pl.step(0.002)
    return pl.apogee_t


def simple_one(arg):
    name, sn, sg, seed, eject = arg
    site = COLD if sn == 'cold' else HOT
    rk = ROCKETS[name]
    if eject:
        eject = (apogee_time(rk, site) + eject[0], eject[1], eject[2])
    from apogee_fallback import Watch
    fs = {v: Simple(*v) for v in simple_variants()}
    first = {}
    w = Watch(pad_temp_err=0.0)
    w.f.true_pad_temp_c = site.t0 - 273.15
    rng = random.Random(seed * 7919)
    t = 0.0
    while t < 12.0:
        w.step(float(int(site.pressure(0.0) + rng.gauss(0, sg))), t); t += DT
    pl = None
    for tf, p, pl in sensed_flight(rk, site, sg, None, seed, eject):
        w.step(p, tf + 12.0)
        if ('normal',) in w.first and 'kalman' not in first:
            first['kalman'] = tf
        for v, f in fs.items():
            if f.step(p, tf) and v not in first:
                first[v] = tf
        if len(first) == len(fs) + 1 and pl.apogee:
            break
    while not pl.apogee and not pl.landed: pl.step(0.002)
    return arg, {v: (first[v] - pl.apogee_t) if v in first else None for v in list(fs) + ['kalman']}


def simple_args():
    out = []
    for name in SIMPLE:
        for sn in ('cold', 'hot'):
            for sg in (1.2, 9.0):
                for seed in range(1, 9):
                    out.append((name, sn, sg, seed, None))
                    for at in EJECT_AT:
                        for pa in EJECT_PA:
                            for tau in EJECT_TAU:
                                out.append((name, sn, sg, seed, (at, pa, tau)))
    return out


def summary(ls):
    got = [l for l in ls if l is not None]
    early = [l for l in got if l < 0]
    late = sorted(l for l in got if l >= 0)
    never = sum(1 for l in ls if l is None)
    pick = lambda q: late[min(int(len(late) * q), len(late) - 1)] if late else float('nan')
    return len(early), min(early, default=0.0), never, pick(0.5), pick(0.9), late[-1] if late else float('nan')


def simple():
    with Pool() as pool:
        rs = pool.map(simple_one, simple_args(), chunksize=8)
    quiet = [r for r in rs if r[0][4] is None]
    spiked = [r for r in rs if r[0][4] is not None]
    print(f"simple flights ({', '.join(SIMPLE)}): {len(quiet)} without an ejection spike, {len(spiked)} with one\n")
    print(f"{'median, rise, hold':22s} | {'no spike: early never med p90 max':38s} | spike: early worst never  max late")
    for v in simple_variants():
        a = summary([r[1][v] for r in quiet])
        b = summary([r[1][v] for r in spiked])
        print(f"{v[0] * DT:4.2f} s {v[1]:4.0f} m {v[2]:4.1f} s   | {a[0]:5d} {a[2]:5d} {a[3]:5.2f} {a[4]:5.2f} {a[5]:5.2f}"
              f"            | {b[0]:5d} {b[1]:+6.2f} {b[2]:5d} {b[5]:6.2f}")
    a = summary([r[1]['kalman'] for r in quiet]); b = summary([r[1]['kalman'] for r in spiked])
    print(f"{'Kalman, firmware rule':22s} | {a[0]:5d} {a[2]:5d} {a[3]:5.2f} {a[4]:5.2f} {a[5]:5.2f}            | {b[0]:5d} {b[1]:+6.2f} {b[2]:5d} {b[5]:6.2f}")
    bad = sorted({r[0][4] for r in spiked if r[1]['kalman'] is not None and r[1]['kalman'] < 0})
    print("  Kalman early with:", ", ".join(f"{a:+g} s {p / 1000:g} kPa tau {t:g}" for a, p, t in bad))
    print("\nearly with a spike, by when it came (s from apogee) and its size:")
    for v in simple_variants():
        bad = sorted({(r[0][4][0], r[0][4][1], r[0][4][2]) for r in spiked if r[1][v] is not None and r[1][v] < 0})
        if bad:
            print(f"  {v[0] * DT:4.2f} s {v[1]:4.0f} m {v[2]:4.1f} s: " +
                  ", ".join(f"{a:+g} s {p / 1000:g} kPa tau {t:g}" for a, p, t in bad[:8]) + (" ..." if len(bad) > 8 else ""))


# ── a simple lockout: the measured speed below V for H, then latched off ──
LOCK_V = (50.0, 100.0, 150.0)
LOCK_H = (1.0, 2.0, 3.0)


class Lockout:
    def __init__(self, v, h):
        self.v, self.h = v, h
        self.since = None
        self.released = False

    def step(self, speed, t):
        if self.released or speed is None:
            return self.released
        if abs(speed) < self.v:
            if self.since is None: self.since = t
            self.released = t - self.since >= self.h
        else:
            self.since = None
        return self.released


def lockout_one(arg):
    kind, name, sn, pn, sg, seed, port = arg
    site = COLD if sn == 'cold' else HOT
    lum = Lumped(pad_temp_err=0.0)
    lum.true_pad_temp_c = site.t0 - 273.15
    rng = random.Random(seed * 7919)
    t = 0.0
    while t < 12.0:
        lum.update(float(int(site.pressure(0.0) + rng.gauss(0, sg))), t); t += DT
    f = Simple(*CHOSEN)
    locks = {(v, h): Lockout(v, h) for v in LOCK_V for h in LOCK_H}
    alone, handed = {}, {}
    pl = None
    for tf, p, pl in sensed_flight(ROCKETS[name], site, sg, port, seed):
        lum.update(p, tf + 12.0)
        met = f.step(p, tf)
        explained = lum.x is not None and lum.explains(4.0)
        for k, lk in locks.items():
            free = lk.step(f.speed, tf)
            if met and free:
                alone.setdefault(k, tf)
                if not explained: handed.setdefault(k, tf)
        if pl.apogee and (tf - pl.apogee_t > 60.0 or len(alone) == len(locks)):
            break
    while not pl.apogee and not pl.landed: pl.step(0.002)
    rel = lambda d: {k: (d[k] - pl.apogee_t) if k in d else None for k in locks}
    return (kind, name, sn, pn, sg, seed), rel(alone), rel(handed), pl.max_mach


def lockout():
    from apogee_fallback import flight_args
    with Pool() as pool:
        rs = pool.map(lockout_one, flight_args(), chunksize=4)
    w, d, h = CHOSEN
    print(f"simple filter: median of {w * DT:g} s, {d:g} m over its lowest, held {h:g} s; lockout: measured speed under V for H\n")
    for label, sel in (('subsonic (max Mach < 0.8)', lambda r: r[3] < 0.8), ('Mach 0.8 and over, clean ports', lambda r: r[3] >= 0.8 and r[0][3] == 'clean'),
                       ('Mach 0.8 and over, port errors', lambda r: r[3] >= 0.8 and r[0][3] != 'clean')):
        part = [r for r in rs if sel(r)]
        print(f"{label}: {len(part)} flights")
        print(f"  {'V, H':14s} {'alone: early worst never late med max':40s}  handover while unexplained: early worst")
        for v in LOCK_V:
            for hh in LOCK_H:
                a = summary([r[1][(v, hh)] for r in part]); b = summary([r[2][(v, hh)] for r in part])
                print(f"  {v:4.0f} m/s {hh:3.0f} s   {a[0]:5d} {a[1]:+7.2f} {a[2]:5d} {a[3]:6.2f} {a[5]:6.2f}              {b[0]:5d} {b[1]:+7.2f}")
        print()


def lockout_chamber(path):
    rows = []
    for line in open(os.path.expanduser(path)):
        if line[0] == '#' or line.startswith('time'): continue
        x = line.rstrip('\n').split(',')
        if x[1] == '' or not x[5]: continue
        rows.append((int(x[0]) / 1000.0, int(x[5])))
    for v in LOCK_V:
        for hh in LOCK_H:
            f = Simple(*CHOSEN); lk = Lockout(v, hh)
            at = next((t for t, p in rows if f.step(p, t) & lk.step(f.speed, t)), None)
            print(f"  lockout {v:4.0f} m/s for {hh:g} s: " + (f"{at:.2f} s" if at else "never"))


# ── handover: the model filter decides until it has not explained for T ──
CHOSEN = (3, 2.0, 0.5)


def handover_one(arg):
    kind, name, sn, pn, sg, seed, port = arg
    site = COLD if sn == 'cold' else HOT
    rk = ROCKETS[name]
    lum = Lumped(pad_temp_err=0.0)
    lum.true_pad_temp_c = site.t0 - 273.15
    t = 0.0
    p0 = site.pressure(0.0)
    rng = random.Random(seed * 7919)
    while t < 12.0:
        lum.update(float(int(p0 + rng.gauss(0, sg))), t); t += DT
    simple_f = Simple(*CHOSEN)
    unexplained_since = None
    first = {}
    longest = 0.0
    simple_alone = None
    converged = {X: False for X in NEVER_AFTER}
    never = {}
    gated = {}
    pl = None
    for tf, p, pl in sensed_flight(rk, site, sg, port, seed):
        lum.update(p, tf + 12.0)
        met = simple_f.step(p, tf)
        if met and simple_alone is None: simple_alone = tf
        if lum.x is None: continue
        if lum.explains(4.0):
            unexplained_since = None
        elif unexplained_since is None:
            unexplained_since = tf
        run = 0.0 if unexplained_since is None else tf - unexplained_since
        for V in SPEED_GATES:
            if met and not lum.explains(4.0) and simple_f.max_climb < V and V not in gated:
                gated[V] = tf
        for X in NEVER_AFTER:
            if tf >= X and lum.explains(4.0):
                converged[X] = True
            if met and tf >= X and not converged[X] and X not in never:
                never[X] = tf
        longest = max(longest, run if not pl.apogee else 0.0)
        for T in NOT_CONVERGED:
            if met and run >= T and T not in first:
                first[T] = tf
        if pl.apogee and len(first) == len(NOT_CONVERGED) and simple_alone is not None and all(
                converged[X] or X in never for X in NEVER_AFTER):
            break
        if pl.apogee and tf - pl.apogee_t > 60.0:
            break
    while not pl.apogee and not pl.landed: pl.step(0.002)
    rel = lambda x: None if x is None else x - pl.apogee_t
    return ((kind, name, sn, pn, sg, seed), {T: rel(first.get(T)) for T in NOT_CONVERGED}, rel(simple_alone), longest,
            pl.max_mach, {X: rel(never.get(X)) for X in NEVER_AFTER}, {V: rel(gated.get(V)) for V in SPEED_GATES},
            simple_f.max_climb)


def handover_args():
    from apogee_fallback import flight_args
    return flight_args()


def handover():
    with Pool() as pool:
        rs = pool.map(handover_one, handover_args(), chunksize=4)
    w, d, h = CHOSEN
    print(f"simple filter: median of {w * DT:g} s, {d:g} m over its lowest, held {h:g} s\n")
    for label, sel in (('subsonic (max Mach < 0.8)', lambda r: r[4] < 0.8), ('Mach 0.8 and over', lambda r: r[4] >= 0.8)):
        part = [r for r in rs if sel(r)]
        a = summary([r[2] for r in part])
        print(f"{label}: {len(part)} flights")
        print(f"  simple filter alone:            early {a[0]:4d} worst {a[1]:+7.2f} never {a[2]:3d} late med {a[3]:.2f} max {a[5]:.2f}")
        for T in NOT_CONVERGED:
            b = summary([r[1][T] for r in part])
            print(f"  handed over after {T:4.0f} s unexplained: early {b[0]:4d} worst {b[1]:+7.2f} never {b[2]:3d} (never = the model kept converging)")
        for X in NEVER_AFTER:
            c = summary([r[5][X] for r in part])
            print(f"  handed over if never explained from {X:3.1f} s after launch: early {c[0]:4d} worst {c[1]:+7.2f} handed over {len(part) - c[2]:4d}")
        for V in SPEED_GATES:
            c = summary([r[6][V] for r in part])
            print(f"  handed over while unexplained, climb measured under {V:3.0f} m/s: early {c[0]:4d} worst {c[1]:+7.2f} handed over {len(part) - c[2]:4d}")
        mc = sorted(r[7] for r in part)
        print(f"  fastest climb the simple filter measured: min {mc[0]:.0f} median {mc[len(mc) // 2]:.0f} max {mc[-1]:.0f} m/s")
        lg = sorted(r[3] for r in part)
        print(f"  longest unexplained run before apogee: median {lg[len(lg) // 2]:.2f} s, max {lg[-1]:.2f} s\n")
    worst = sorted((r[1][T], T, r[0]) for r in rs for T in NOT_CONVERGED if r[1][T] is not None and r[1][T] < 0)
    if worst:
        print("earliest handovers:")
        for l, T, c in worst[:10]: print(f"  after {T:g} s: {l:+.2f} s {c}")


def chamber(path):
    rows, ground = [], None
    for line in open(os.path.expanduser(path)):
        if line.startswith('# Ground Pa:'): ground = int(line.split(':')[1])
        if line[0] == '#' or line.startswith('time'): continue
        f = line.rstrip('\n').split(',')
        if f[1] == '' or not f[5]: continue
        rows.append((int(f[0]) / 1000.0, int(f[5])))
    lowest = min(rows, key=lambda r: r[1])
    height = lambda p: 44330.8 * (1 - (p / ground) ** 0.190263)
    print(f"chamber log: lowest raw reading {lowest[1]} Pa ({height(lowest[1]):.0f} m) at {lowest[0]:.2f} s")
    for v in simple_variants():
        f = Simple(*v)
        f.ground = ground
        at = next((t for t, p in rows if f.step(p, t)), None)
        tail = f"{at:6.2f} s, {at - lowest[0]:+5.2f} s from the lowest reading, {height(min(rows, key=lambda r: abs(r[0] - at))[1]):5.0f} m" if at else "never"
        print(f"  median {v[0] * DT:4.2f} s, {v[1]:3.0f} m, hold {v[2]:3.1f} s: {tail}")


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'simple'
    if what == 'chamber':
        chamber(sys.argv[2])
        lockout_chamber(sys.argv[2])
    elif what == 'lockout':
        lockout()
    elif what == 'handover':
        handover()
    else:
        simple()
