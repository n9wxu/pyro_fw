"""Fallbacks for apogee when the model never explains the readings.

    python3 sim/study/apogee_fallback.py [flights|chamber <flight.csv>]

Each flight is flown once with nothing declared, so the plant coasts over the
top untouched, and every candidate records when it would first declare. A
candidate is judged by when it would declare on its own: early on any flight
whose readings stayed possible rules it out. The chamber case feeds a logged
flight's raw readings through the same filter.
"""
import math, os, sys
from collections import deque
from multiprocessing import Pool
from lumped_fly import *
from lumped import Lumped
from lumped_tune import cases
from est_proto import Rocket

# The board's ceiling is 30,000 ft (9,144 m): higher needs a quieter sensor or
# GPS. The low-drag rocket trimmed to peak just under it from the hot pad.
ROCKETS['30 kft'] = Rocket(8.0, 3.6, 1216.0, 4.5, 0.0008, 1.5)
IN_RANGE = ['hop', 'subsonic', 'mid-Mach', 'draggy', '30 g', '30 kft']
FAST = ['draggy', '30 g', '30 kft']

ARM_MS, ARM_H = 10.0, 30.0
WINDOW = 50            # readings in the model-free mean: 1 s at 50/s
SCALE_H = 8000.0       # m per e-fold, for a height margin in pascals
HOLDS = (1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 10.0)
RISES = (10.0, 30.0)
FORGIVE = (0.5, 1.0)
LONG_HOLDS = (6.0, 8.0, 10.0, 12.0, 15.0)


def variants():
    out = [('normal',)]
    out += [('unexplained', T) for T in HOLDS]
    out += [('forgiving', G, T) for G in FORGIVE for T in LONG_HOLDS]
    out += [('trend', D, T) for D in RISES for T in HOLDS]
    return out


class Watch(Flight):
    """The firmware's rule and every candidate, side by side; declares nothing."""

    def __init__(self, **tune):
        super().__init__(**tune)
        self.first = {}
        self.max_v = 0.0
        self.armed = False
        self.fall_since = None
        self.run_since = {G: None for G in FORGIVE}
        self.last_fall = None
        self.raw = deque(maxlen=WINDOW)
        self.min_mean = None
        self.above_since = {D: None for D in RISES}

    def note(self, key, t):
        self.first.setdefault(key, t)

    def step(self, p_raw, t, loosen_beta=0.0):
        f = self.f
        f.update(p_raw, t, loosen_beta)
        if f.x is None:
            return
        if self.state == 'PAD':
            if f.h() > 30.48 and f.v() > 5.0:
                self.state = 'ASCENT'; self.launch_t = t
            return
        self.max_v = max(self.max_v, f.v())
        if not self.armed and self.max_v >= ARM_MS and f.v() < ARM_MS and f.h() > ARM_H:
            self.armed = True
        if p_raw is not None:
            self.raw.append(p_raw)
        if not self.armed:
            return
        self.normal(t)
        self.unexplained(t)
        self.forgiving(t)
        self.trend(t)

    def normal(self, t):
        f = self.f
        explained = f.explains(self.fit_limit)
        margin = self.apogee_sigmas * f.sig_v()
        if not explained:
            self.climb_seen = False; self.falling_since = None
            return
        if f.v() > margin:
            self.climb_seen = True
        if not f.v() < -margin:
            self.falling_since = None
            return
        if self.falling_since is None: self.falling_since = t
        if self.climb_seen or t - self.falling_since >= self.unseen_s:
            self.note(('normal',), t)

    def unexplained(self, t):
        f = self.f
        if f.v() < -self.apogee_sigmas * f.sig_v():
            if self.fall_since is None: self.fall_since = t
            for T in HOLDS:
                if t - self.fall_since >= T:
                    self.note(('unexplained', T), t)
        else:
            self.fall_since = None

    def forgiving(self, t):
        f = self.f
        if not f.v() < -self.apogee_sigmas * f.sig_v():
            return
        for G in FORGIVE:
            if self.run_since[G] is None or t - self.last_fall > G:
                self.run_since[G] = t
            for T in LONG_HOLDS:
                if t - self.run_since[G] >= T:
                    self.note(('forgiving', G, T), t)
        self.last_fall = t

    def trend(self, t):
        if len(self.raw) < WINDOW:
            return
        mean = sum(self.raw) / WINDOW
        if self.min_mean is None or mean < self.min_mean:
            self.min_mean = mean
            for D in RISES: self.above_since[D] = None
            return
        for D in RISES:
            if mean > self.min_mean * (1.0 + D / SCALE_H):
                if self.above_since[D] is None: self.above_since[D] = t
                for T in HOLDS:
                    if t - self.above_since[D] >= T:
                        self.note(('trend', D, T), t)
            else:
                self.above_since[D] = None


def watched(rk, site, sigma, port, seed):
    import lumped_fly
    saved = lumped_fly.Flight
    lumped_fly.Flight = Watch
    try:
        r = fly(rk, site=site, sigma=sigma, port=port, seed=seed, tune={'pad_temp_err': 0.0}, stop_at_apogee=False,
                until=300, lights=False)
    finally:
        lumped_fly.Flight = saved
    w = r['filter']
    return r, w


# fly() keeps the filter, not the detector; Watch hands itself back through it.
_orig_init = Watch.__init__
def _init(self, **tune):
    _orig_init(self, **tune)
    self.f.watch = self
Watch.__init__ = _init


def one(arg):
    kind, name, sn, pn, sg, seed, port = arg
    site = COLD if sn == 'cold' else HOT
    r, f = watched(ROCKETS[name], site, sg, port, seed)
    w = f.watch
    pad_s = 12.0
    out = {v: (w.first[v] - pad_s - r['true_apogee_t']) if v in w.first else None for v in variants()}
    return kind, (name, sn, pn, sg, seed), out, r.get('impossible_s', 0.0)


def flight_args():
    args = [('in range',) + c + (PORTS[c[2]],) for c in cases(IN_RANGE, 8)]
    b, a, s = PORTS['fake'][1:]
    for name in FAST:
        for sign in (-1, 1):
            for k in (0.1, 0.25, 0.5, 1.0, 2.0, 4.0):
                for sg in (1.2, 9.0):
                    for seed in (1, 2, 3):
                        args.append(('port scale', name, 'cold', f"{sign * k:+g}", sg, seed,
                                     (float(sign), b * k, a * k, s * k)))
    return args


def label(v):
    if v[0] == 'normal': return 'firmware rule (FLT-APO-07)'
    if v[0] == 'unexplained': return f'unexplained fall held {v[1]:g} s'
    if v[0] == 'forgiving': return f'fall held {v[2]:g} s, breaks < {v[1]:g} s ok'
    return f'pressure {v[1]:g} m over its lowest, held {v[2]:g} s'


def flights():
    with Pool() as pool:
        rs = pool.map(one, flight_args(), chunksize=4)
    possible = [r for r in rs if r[3] < 0.5]
    impossible = [r for r in rs if r[3] >= 0.5]
    print(f"{len(rs)} flights: {len(possible)} with readings a sensor can give, {len(impossible)} with the error past the air's own pressure\n")
    print(f"{'candidate':40s} {'early':>6s} {'worst':>8s} {'never':>6s} {'late med':>9s} {'p90':>7s} {'max':>7s}")
    for v in variants():
        ls = [r[2][v] for r in possible]
        got = [l for l in ls if l is not None]
        early = [l for l in got if l < 0]
        late = sorted(l for l in got if l >= 0)
        never = sum(1 for l in ls if l is None)
        med = late[len(late) // 2] if late else float('nan')
        p90 = late[int(len(late) * 0.9)] if late else float('nan')
        mx = late[-1] if late else float('nan')
        print(f"{label(v):40s} {len(early):6d} {min(early, default=0):+8.2f} {never:6d} {med:9.2f} {p90:7.2f} {mx:7.2f}")
    print("\nearliest declarations, possible readings only:")
    for v in variants()[1:]:
        worst = sorted((r[2][v], r[1]) for r in possible if r[2][v] is not None and r[2][v] < 0)[:3]
        if worst:
            print(f"  {label(v):40s} " + "; ".join(f"{l:+.2f} s {c}" for l, c in worst))


def chamber(path):
    rows = []
    ground = None
    for line in open(os.path.expanduser(path)):
        if line.startswith('# Ground Pa:'):
            ground = int(line.split(':')[1])
        if line[0] == '#' or line.startswith('time'):
            continue
        f = line.rstrip('\n').split(',')
        if f[1] == '' or not f[5]:
            continue
        rows.append((int(f[0]) / 1000.0, int(f[5]), float(f[6])))
    w = Watch(pad_temp_err=0.0)
    w.f.true_pad_temp_c = rows[0][2]
    pad_s = 10.0
    t = 0.0
    while t < pad_s:
        w.step(ground, t); t += DT
    launch = None
    for tl, p, _ in rows:
        w.step(p, tl + pad_s)
        if launch is None and w.state == 'ASCENT': launch = tl
    print(f"chamber log {path}: {len(rows)} readings, ground {ground} Pa, filter launch at {launch} s, armed {w.armed}")
    lowest = min(rows, key=lambda r: r[1])
    print(f"lowest raw reading {lowest[1]} Pa at {lowest[0]:.2f} s\n")
    for v in variants():
        at = w.first.get(v)
        if at is None:
            print(f"  {label(v):40s} never")
            continue
        at -= pad_s
        p = min(rows, key=lambda r: abs(r[0] - at))[1]
        h = 44330.8 * (1 - (p / ground) ** 0.190263)
        print(f"  {label(v):40s} {at:7.2f} s, {at - lowest[0]:+6.2f} s from the lowest reading, {h:6.0f} m")


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'chamber':
        chamber(sys.argv[2])
    else:
        flights()
