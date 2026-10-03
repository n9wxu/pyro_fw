"""The complete study of the lumped-parameter filter (HA-1, option E).

    python3 sim/study/lumped_study.py [section ...]     sections: sweep track params pad descent failures cost

Each section prints a block of docs/lumped_parameter_filter.md.
"""
import math, random, sys, time
from multiprocessing import Pool
from est_proto import Est, Site, Rocket, Plant, port_error, scale_height, G, R_AIR
from lumped import Lumped, std_altitude, std_pressure
from lumped_fly import *
import est_fly

import os, json
T = {'pad_temp_err': 0.0}   # the filter given the pad's temperature
T.update(json.loads(os.environ.get('LUMPED_TUNE', '{}')))
FAST = ['draggy', 'low-drag', '30 g', '20 km', '30 km']
SCALES = [0.1, 0.25, 0.5, 1.0, 2.0, 4.0]


# ── the port-error sweep ───────────────────────────────────────────────
def sweep_one(arg):
    name, sign, k, sg, seed = arg
    b, a, s = PORTS['fake'][1:]
    port = (float(sign), b * k, a * k, s * k)
    r = fly(ROCKETS[name], site=COLD, sigma=sg, port=port, seed=seed, tune=T)
    return name, sign * k, r.get('late'), 'apogee' in r, r.get('impossible_s', 0.0)

def sweep(pool):
    args = [(n, sg_, k, sg, seed) for n in FAST for sg_ in (-1, 1) for k in SCALES for sg in (1.2, 9.0) for seed in (1, 2, 3)]
    rs = pool.map(sweep_one, args, chunksize=4)
    early = [r for r in rs if r[3] and r[2] < 0]
    never = [r for r in rs if not r[3]]
    lates = sorted(r[2] for r in rs if r[3] and r[2] >= 0)
    print(f"port sweep: {len(rs)} flights, early {len(early)}, never {len(never)}, late median {lates[len(lates)//2]:.2f} max {lates[-1]:.2f} s")
    for name in FAST:
        row = []
        for sc in [-k for k in reversed(SCALES)] + SCALES:
            ls = [r[2] for r in rs if r[0] == name and r[1] == sc and r[3]]
            row.append(f"{sc:+g}:{max(ls):+.2f}" if ls else f"{sc:+g}:never")
        print(f"  {name:9s} " + " ".join(row))
    print(f"  every early flight had readings the sensor cannot produce (the modelled error exceeding the air's own pressure) for {min((r[4] for r in early), default=0):.0f} to {max((r[4] for r in early), default=0):.0f} s")
    ok = [r for r in rs if r[4] < 0.5]
    print(f"  flights whose readings stayed possible: {len(ok)}, early {sum(1 for r in ok if r[3] and r[2] < 0)}")


# ── tracking: the lumped filter beside the kinematic one ──────────────
def track_one(arg):
    name, sn, sg, seed, which = arg
    site = COLD if sn == 'cold' else HOT
    rk = ROCKETS[name]
    rng = random.Random(seed)
    pl = Plant(site, rk)
    lum = Lumped(**{k: v for k, v in T.items() if k not in ('over_the_top', 'unseen_s', 'apogee_sigmas', 'fit_limit', 'apogee_ms')}); lum.true_pad_temp_c = site.t0 - 273.15
    kin = Est(1.2)
    pad_s = 12.0
    err = {'lum': {'coast': [], 'top': []}, 'kin': {'coast': [], 'top': []}}
    n = 0
    p0 = site.pressure(0.0)
    while not pl.apogee or pl.t < pl.apogee_t + 0.5:
        t = n * DT; n += 1
        tf = t - pad_s
        if tf >= 0:
            for _ in range(20): pl.step(0.001)
        p = float(int(site.pressure(pl.h) + rng.gauss(0, sg)))
        lum.update(p, t); kin.update(p, t)
        if tf > rk.burn + 1.0 and kin.x is not None:
            # in the filter's own terms: pressure altitude and its rate
            h_true = std_altitude(site.pressure(pl.h)) - std_altitude(p0) if which != 'T' else pl.h
            scale = (h_true - track_one.prev_h) / DT if hasattr(track_one, 'prev_h') else pl.v
            z, zd, _ = kin.x
            pk = math.exp(z)
            if which == 'T':
                air = lum.air
                # the kinematic filter's height and speed through the same air
                hk = air and _height(air, pk) or 0.0
                vk = -zd * R_AIR * air.temp(hk) / G
            else:
                hk = std_altitude(pk) - std_altitude(p0)
                vk = -zd * scale_height(pk)
            phase = 'top' if pl.t > (pl.apogee_t - 3.0 if pl.apogee else 1e9) or (pl.v < 30.0) else 'coast'
            v_true = pl.v if which == 'T' else pl.v * _std_stretch(site, pl.h)
            err['lum'][phase].append((lum.h() - h_true, lum.v() - v_true))
            err['kin'][phase].append((hk - h_true, vk - v_true))
    return name, sn, sg, which, err

def _height(air, p):
    lo, hi = -500.0, 60000.0
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if air.pressure(mid) > p: lo = mid
        else: hi = mid
    return 0.5 * (lo + hi)

def _std_stretch(site, h):
    """d(standard pressure altitude)/d(true height): what a speed becomes in the standard atmosphere."""
    p = site.pressure(h)
    return (site.temp(h) and (R_AIR * _isa(p) / G) / (R_AIR * site.temp(h) / G))

def _isa(p):
    from est_proto import isa_T
    return isa_T(p)

def rms(xs): return math.sqrt(sum(x * x for x in xs) / len(xs)) if xs else 0.0

def track(pool):
    print("speed error, m/s rms, clean ports (kinematic = the filter flying today):")
    print("  rocket    noise   coast: lumped  kinematic |  last 3 s to apogee: lumped  kinematic")
    args = [(n, 'cold', sg, seed, 'T') for n in ROCKETS if n != '45 km' for sg in (1.2, 9.0) for seed in (1, 2, 3, 4)]
    rs = pool.map(track_one, args, chunksize=2)
    tot = []
    for name in [n for n in ROCKETS if n != '45 km']:
        for sg in (1.2, 9.0):
            sel = [r[4] for r in rs if r[0] == name and r[2] == sg]
            def col(f, ph): return rms([e[1] for s_ in sel for e in s_[f][ph]])
            print(f"  {name:9s} {sg:4.1f} Pa   {col('lum','coast'):9.2f} {col('kin','coast'):10.2f} | {col('lum','top'):18.2f} {col('kin','top'):10.2f}")
            tot.append((col('lum','coast'), col('kin','coast'), col('lum','top'), col('kin','top')))
    m = [sum(t[i] for t in tot) / len(tot) for i in range(4)]
    print(f"  mean                {m[0]:9.2f} {m[1]:10.2f} | {m[2]:18.2f} {m[3]:10.2f}")


# ── what the parameters recover ───────────────────────────────────────
def params_one(arg):
    name, sg, seed = arg
    site = COLD
    rk = ROCKETS[name]
    tr = []
    r = fly(rk, site=site, sigma=sg, seed=seed, tune=T, trace=tr, until=400, stop_at_apogee=False, main=5.0, main_agl=300.0)
    ta = r['true_apogee_t']
    rho_pad = site.p0 / (R_AIR * site.t0)
    beta_true = rho_pad * rk.cda / (2.0 * rk.dry)
    coast = [row[6] for row in tr if rk.burn + 0.5 * (ta - rk.burn) < row[0] < ta - 2.0 and abs(row[10]) < 0.8]
    early = [row[5] for row in tr if 0.1 < row[0] < 0.4 * rk.burn]
    thrust_true = rk.thrust / (rk.dry + rk.prop)
    drogue = [row[6] for row in tr if row[0] > ta + 8.0 and row[1] > 350.0]
    mainp = [row[6] for row in tr if row[1] < 200.0 and row[0] > ta + 8.0 and row[1] > 20.0]
    def vt(bs): return math.sqrt(G / (sum(bs) / len(bs))) if bs else float('nan')
    return (name, sg, vt(coast), math.sqrt(G / beta_true), (sum(early) / len(early)) if early else float('nan'), thrust_true,
            vt(drogue), vt(mainp))

def params(pool):
    print("what the two parameters recover, cold pad, clean ports (terminal speed in the pad's air = sqrt(g / beta)):")
    print("  rocket    noise | airframe terminal m/s: est  true | thrust/mass early burn m/s2: est  true | canopy 20 m/s: est | main 5 m/s: est")
    args = [(n, sg, 1) for n in ROCKETS if n != '45 km' for sg in (1.2, 9.0)]
    for r in pool.map(params_one, args):
        print(f"  {r[0]:9s} {r[1]:4.1f} Pa | {r[2]:22.0f} {r[3]:5.0f} | {r[4]:30.0f} {r[5]:5.0f} | {r[6]:18.1f} | {r[7]:14.1f}")


# ── the pad ───────────────────────────────────────────────────────────
def pad_one(arg):
    hours, sg, gust, seed, glitch = arg
    rng = random.Random(seed)
    d = Flight(**T); d.f.true_pad_temp_c = 15.0
    g = 0.0
    keep = math.exp(-DT / 3.0); drive = gust * math.sqrt(1 - keep * keep)
    launches = 0; vmax = hmax = 0.0
    n = int(hours * 3600 / DT)
    for i in range(n):
        g = keep * g + drive * rng.gauss(0, 1)
        p = 101325.0 + g + rng.gauss(0, sg)
        if glitch and i > 3000 and (i % 3000) < glitch[1]: p += glitch[0]
        d.step(float(int(p)), i * DT)
        if d.state != 'PAD':
            launches += 1; d.state = 'PAD'
        if i > 500:
            vmax = max(vmax, abs(d.f.v())); hmax = max(hmax, abs(d.f.h()))
    return sg, gust, glitch, launches, vmax, hmax

def pad(pool):
    print("on the pad (a launch is 100 ft and 5 m/s on the filtered state):")
    args = [(1.0, sg, 30.0, seed, None) for sg in (1.2, 9.0) for seed in (1, 2, 3, 4)]
    args += [(0.25, sg, 0.0, 1, (amp, nbad)) for sg in (1.2, 9.0) for amp in (-60000, -12000, -3000, -300, 300, 20000) for nbad in (1, 2)]
    rs = pool.map(pad_one, args)
    for sg in (1.2, 9.0):
        sel = [r for r in rs if r[0] == sg and r[2] is None]
        print(f"  {len(sel)} h of 30 Pa gusts at {sg} Pa noise: false launches {sum(r[3] for r in sel)}, largest speed {max(r[4] for r in sel):.2f} m/s, largest height {max(r[5] for r in sel):.1f} m")
        sel = [r for r in rs if r[0] == sg and r[2] is not None]
        print(f"  one or two bad readings every minute, 300 Pa to 60 kPa off, at {sg} Pa: false launches {sum(r[3] for r in sel)}, largest speed {max(r[4] for r in sel):.2f} m/s, largest height {max(r[5] for r in sel):.1f} m")
    print("  launch declared at (true height, m):", ", ".join(f"{n} {fly(ROCKETS[n], site=COLD, sigma=9.0, tune=T)['launch_h']:.0f}" for n in ROCKETS if n != '45 km'))


# ── descent ───────────────────────────────────────────────────────────
def pad_air_speed(f):
    """The descent speed as the pad's air would give it, from the state."""
    p = f.pressure_at(f.h())
    return -f.v() * math.sqrt((p / f.temp_at(f.h(), p)) / f.rho_pad)

def descent_one(arg):
    label, name, sg, seed, kw = arg
    site = COLD
    tr = []
    stats = {'label': label, 'sg': sg}
    rng_fly = dict(site=site, sigma=sg, seed=seed, tune=T, until=600, stop_at_apogee=False, trace=tr)
    rng_fly.update(kw)
    # the trace has no pad-air speed: recompute from a second pass with the filter kept
    r = fly(ROCKETS[name], **rng_fly)
    ta = r['true_apogee_t']
    rho_pad = site.p0 / (R_AIR * site.t0)
    errs = []; worst = 0.0; seen = None; top = 0.0
    for row in tr:
        tf, h, v, he, ve, aT, beta, fit, sv, nu, mach = row
        if tf < ta + 1.0 or h < 5.0: continue
        rho = site.pressure(h) / (R_AIR * site.temp(h))
        true_pa = -v * math.sqrt(rho / rho_pad)
        est_pa = -ve * math.sqrt(rho / rho_pad)
        top = max(top, est_pa)
        if tf > ta + 6.0: errs.append(est_pa - true_pa)
        if 'lost' in r and tf > r['lost'] and seen is None and est_pa > 35.0: seen = tf - r['lost']
    stats['rms'] = rms(errs); stats['max'] = max((abs(e) for e in errs), default=0.0)
    stats['seen'] = seen; stats['top'] = top
    stats['late'] = r.get('late')
    return stats

def descent(pool):
    print("descent speed in the pad's air, which the fire rules read (error after the first 6 s):")
    cases = [
        ('canopy at 20 m/s, subsonic', 'subsonic', {}),
        ('canopy at 20 m/s, from 20 km (thin air)', '20 km', {}),
        ('canopy at 20 m/s, from 30 km (thin air)', '30 km', {}),
        ('drogue 20 then main 5 m/s at 300 m', 'subsonic', {'main': 5.0}),
        ('no canopy at all (ballistic)', 'subsonic', {'lights': False}),
        ('no canopy at all, from 20 km', '20 km', {'lights': False}),
        ('canopy lost at 500 m', 'low-drag', {'lost_below': 500.0}),
        ('bay charge 3 kPa at each fire', 'subsonic', {'charge': (3000.0, 0.2), 'main': 5.0}),
        ('bay charge 3 kPa, readings used at once', 'subsonic', {'charge': (3000.0, 0.2), 'main': 5.0, 'blank_s': 0.0}),
    ]
    args = [(lab, n, sg, seed, kw) for lab, n, kw in cases for sg in (1.2, 9.0) for seed in (1, 2)]
    rs = pool.map(descent_one, args)
    for lab, _, _ in cases:
        for sg in (1.2, 9.0):
            sel = [r for r in rs if r['label'] == lab and r['sg'] == sg]
            seen = [r['seen'] for r in sel if r['seen'] is not None]
            extra = f", 35 m/s seen {max(seen):.1f} s after the loss" if seen else ""
            print(f"  {lab:42s} {sg:4.1f} Pa: rms {max(r['rms'] for r in sel):5.2f} m/s, worst {max(r['max'] for r in sel):6.2f} m/s, fastest reported {max(r['top'] for r in sel):6.1f} m/s{extra}")


# ── sensor failures ───────────────────────────────────────────────────
def fail_one(arg):
    label, name, sg, seed, kw = arg
    rk = ROCKETS[name]
    pl = Plant(COLD, rk)
    while not pl.apogee: pl.step(0.002)
    ta = pl.apogee_t
    kw = {k: ((ta + v[0], v[1]) if k in ('dropout', 'stuck') else (ta + v[0], v[1], v[2]) if k == 'glitch' else v) for k, v in kw.items()}
    r = fly(rk, site=COLD, sigma=sg, seed=seed, tune=T, until=ta + 150, **kw)
    return label, sg, r.get('late'), 'apogee' in r

def failures(pool):
    print("the sensor failing around apogee (times are from the true apogee):")
    cases = [
        ('no readings from -1 s to +1 s', {'dropout': (-1.0, 2.0)}),
        ('no readings from -5 s to +0.5 s', {'dropout': (-5.0, 5.5)}),
        ('no readings from -8 s to +3 s', {'dropout': (-8.0, 11.0)}),
        ('stuck from -3 s for 2 s', {'stuck': (-3.0, 2.0)}),
        ('stuck from -1 s for 3 s', {'stuck': (-1.0, 3.0)}),
        ('two readings 3 kPa high at -0.5 s', {'glitch': (-0.5, 3000.0, 2)}),
        ('two readings 3 kPa low at -0.5 s', {'glitch': (-0.5, -3000.0, 2)}),
        ('ten readings 3 kPa high at -2 s', {'glitch': (-2.0, 3000.0, 10)}),
    ]
    args = [(lab, n, sg, seed, kw) for lab, kw in cases for n in ('subsonic', 'low-drag', '20 km') for sg in (1.2, 9.0) for seed in (1, 2)]
    rs = pool.map(fail_one, args)
    for lab, _ in cases:
        sel = [r for r in rs if r[0] == lab]
        lates = [r[2] for r in sel if r[3]]
        print(f"  {lab:36s}: early {sum(1 for l in lates if l < 0)}, never {sum(1 for r in sel if not r[3])}, apogee declared {min(lates):+.2f} to {max(lates):+.2f} s")


# ── the filter flying today, on the same flights ──────────────────────
def today_one(arg):
    name, sn, pn, sg, seed = arg
    r = est_fly.fly(ROCKETS[name], site=COLD if sn == 'cold' else HOT, sigma=sg, sigma_assumed=1.2, port=PORTS[pn], seed=seed)
    return (name, sn, pn, sg, seed), r.get('apo_late'), 'apogee' in r, r.get('early')

def today(pool):
    from lumped_tune import cases, IN_RANGE
    cs = cases(IN_RANGE, 4)
    rs = pool.map(today_one, cs, chunksize=4)
    lates = sorted(r[1] for r in rs if r[2] and not r[3] and r[1] is not None)
    early = sum(1 for r in rs if r[3])
    clean = sorted(r[1] for r in rs if r[2] and not r[3] and r[0][2] == 'clean' and r[1] is not None)
    print(f"the filter flying today with its Mach flag, same {len(cs)} flights (Python port, sim/study/est_fly.py): early {early}, never {sum(1 for r in rs if not r[2])}, "
          f"late median {lates[len(lates)//2]:.2f} p90 {lates[int(len(lates)*0.9)]:.2f} max {lates[-1]:.2f} s, clean max {clean[-1]:.2f} s")


# ── cost ──────────────────────────────────────────────────────────────
def cost(pool):
    n = 20000
    lum = Lumped(); kin = Est(1.2)
    t0 = time.perf_counter()
    for i in range(n): kin.update(101325.0 + (i % 7), i * DT)
    t1 = time.perf_counter()
    for i in range(n): lum.update(101325.0 + (i % 7), i * DT)
    t2 = time.perf_counter()
    print(f"cost per reading in Python: kinematic {1e6 * (t1 - t0) / n:.1f} us, lumped {1e6 * (t2 - t1) / n:.1f} us: {(t2 - t1) / (t1 - t0):.1f} times")
    print("  state 3 -> 4, covariance 6 -> 10 distinct terms; per reading: about 2 exp, 2 log, 2 pow more than today (density and softplus)")


SECTIONS = {'today': today, 'sweep': sweep, 'track': track, 'params': params, 'pad': pad, 'descent': descent, 'failures': failures, 'cost': cost}
if __name__ == '__main__':
    which = sys.argv[1:] or list(SECTIONS)
    with Pool() as pool:
        for s in which:
            print(f"== {s}")
            SECTIONS[s](pool)
            print(flush=True)
