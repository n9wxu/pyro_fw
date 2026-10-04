"""Fly the lumped-parameter filter beside the truth.

    python3 sim/study/lumped_fly.py trace <rocket> [port]
"""
import math, random, sys
from est_proto import Site, Rocket, Plant, port_error
from lumped import Lumped, G

DT = 0.02
ROCKETS = {
    'hop':      Rocket(0.3, 0.02, 18.0, 0.8, 0.0012, 2.0, 6.0),
    'subsonic': Rocket(1.0, 0.10, 141.0, 1.5, 0.0020, 2.0),
    'mid-Mach': Rocket(1.0, 0.15, 220.0, 1.6, 0.0020, 2.0),
    'draggy':   Rocket(2.0, 0.60, 1709.0, 1.5, 0.0045, 3.0),
    'low-drag': Rocket(8.0, 4.00, 1216.0, 5.0, 0.0008, 1.5),
    '30 g':     Rocket(1.0, 0.30, 395.0, 1.1, 0.0015, 2.0),
    '20 km':    Rocket(8.0, 3.00, 1700.0, 5.0, 0.0008, 1.5),
    '30 km':    Rocket(8.0, 6.00, 2000.0, 6.0, 0.0008, 1.5),
    '45 km':    Rocket(8.0, 8.00, 2100.0, 7.5, 0.0008, 1.5),
}
PORTS = {'clean': None, 'high': (+1.0, 0.02, 0.05, 0.03), 'low': (-1.0, 0.02, 0.05, 0.03),
         'fake': (+1.0, 0.06, 0.14, 0.05)}
COLD, HOT, ISA = Site(10, 0), Site(45, 2000), Site(15, 0)


class Flight:
    """The detectors that read the filter: as few and as plain as they can be."""

    def __init__(self, apogee_sigmas=3.0, fit_limit=4.0, apogee_ms=0.0, over_the_top=True, unseen_s=2.0, **tune):
        self.apogee_sigmas, self.fit_limit, self.apogee_ms = apogee_sigmas, fit_limit, apogee_ms
        self.over_the_top, self.unseen_s = over_the_top, unseen_s
        self.climb_seen = False
        self.falling_since = None
        self.f = Lumped(**tune)
        self.state = 'PAD'
        self.apogee_t = None
        self.launch_t = None

    def step(self, p_raw, t, loosen_beta=0.0):
        f = self.f
        f.update(p_raw, t, loosen_beta)
        if f.x is None:
            return
        if self.state == 'PAD':
            if f.h() > 30.48 and f.v() > 5.0:
                self.state = 'ASCENT'; self.launch_t = t
        elif self.state == 'ASCENT':
            explained = f.explains(self.fit_limit)
            margin = self.apogee_sigmas * f.sig_v() + self.apogee_ms
            falling = explained and f.v() < -margin
            if not self.over_the_top:
                if falling:
                    self.state = 'DESCENT'; self.apogee_t = t
                return
            # Apogee is the explained state going over the top: seen climbing,
            # then seen falling, with the model explaining the readings all the
            # way. A fall whose climb was not seen must last unseen_s.
            if not explained:
                self.climb_seen = False; self.falling_since = None
                return
            if f.v() > margin:
                self.climb_seen = True
            if not falling:
                self.falling_since = None
                return
            if self.falling_since is None: self.falling_since = t
            if self.climb_seen or t - self.falling_since >= self.unseen_s:
                self.state = 'DESCENT'; self.apogee_t = t


def fly(rk, site=ISA, sigma=3.0, port=None, seed=1, pad_s=12.0, canopy=20.0, main=None, main_agl=300.0,
        lights=True, charge=None, glitch=None, dropout=None, until=None, trace=None, tune=None, loosen=4.0,
        stop_at_apogee=True, lost_below=None, stuck=None, blank_s=1.0):
    rng = random.Random(seed)
    pl = Plant(site, rk)
    d = Flight(**(tune or {}))
    d.f.true_pad_temp_c = site.t0 - 273.15
    res = {'early': False}
    n = 0
    fired_t = None
    main_out = False
    last_sensed = site.pressure(0.0)
    same = 0; last_whole = None
    while True:
        t = n * DT; n += 1
        tf = t - pad_s
        if tf >= 0:
            for _ in range(20): pl.step(0.001)
        ps = site.pressure(pl.h)
        sensed = ps + port_error(port, pl.mach, ps) + rng.gauss(0, sigma)
        if charge and fired_t is not None:
            sensed += charge[0] * math.exp(-(tf - fired_t) / charge[1])
        if glitch and glitch[0] <= tf < glitch[0] + glitch[2] * DT: sensed += glitch[1]
        loosen_now = 0.0
        impossible = not (1.0 <= sensed <= 130000.0)   # the HAL discards what the part cannot output
        missing = impossible or (dropout and dropout[0] <= tf < dropout[0] + dropout[1])
        if stuck and stuck[0] <= tf < stuck[0] + stuck[1]:
            sensed = last_sensed
        last_sensed = sensed
        if lost_below and pl.apogee and pl.canopy_ms and pl.h < lost_below and not res.get('lost'):
            pl.canopy_ms = None; res['lost'] = tf
        if fired_t is not None and abs(tf - fired_t) < DT / 2: loosen_now = loosen
        # As the firmware does before its estimator: eight identical readings
        # in a row are a stuck sensor, and are not fed on [SNS-PRES-10].
        whole = float(int(sensed)) if not impossible else None
        same = same + 1 if whole is not None and whole == last_whole else 0
        last_whole = whole
        if same >= 7: missing = True
        # A charge pressurises the bay: for blank_s after the board's own pulse
        # the bay is not the atmosphere, and its readings are not used.
        if fired_t is not None and 0.0 <= tf - fired_t < blank_s: missing = True
        if impossible: res['impossible_s'] = res.get('impossible_s', 0.0) + DT
        d.step(None if missing else whole, t, loosen_now)
        if d.state != 'PAD' and 'launch' not in res:
            res['launch'] = tf; res['launch_h'] = pl.h
        if d.state == 'DESCENT' and 'apogee' not in res:
            res['apogee'] = tf
            res['early'] = not pl.apogee
            res['late'] = tf - pl.apogee_t if pl.apogee else None
            fired_t = tf + DT
            if lights and canopy: pl.canopy_ms = canopy
            if stop_at_apogee and until is None: break
        if main and 'apogee' in res and not main_out and pl.h < main_agl and lights:
            pl.canopy_ms = main; main_out = True; fired_t = tf + DT
        if trace is not None and d.f.x is not None:
            f = d.f
            trace.append((tf, pl.h, pl.v, f.h(), f.v(), f.a_T(), f.beta(), f.fit, f.sig_v(), f.nu, pl.mach))
        if pl.landed or tf > (until or 600): break
    while not pl.apogee and pl.t < 900: pl.step(0.002)   # the truth, for a declaration that came early
    if res.get('early'): res['late'] = res['apogee'] - pl.apogee_t
    res['true_apogee_t'] = pl.apogee_t; res['true_apogee_h'] = pl.apogee_h; res['max_mach'] = pl.max_mach
    res['filter'] = d.f
    return res


if __name__ == '__main__':
    name = sys.argv[2] if len(sys.argv) > 2 else 'subsonic'
    port = PORTS[sys.argv[3]] if len(sys.argv) > 3 else None
    tr = []
    r = fly(ROCKETS[name], port=port, trace=tr, until=80, stop_at_apogee=False)
    print({k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items() if k != 'filter'})
    print("   t      h_true  v_true |  h_est   v_est    a_T     v_term   fit   sig_v    nu   mach")
    for row in tr[::25]:
        tf, h, v, he, ve, aT, beta, fit, sv, nu, mach = row
        if tf < -1: continue
        print(f"{tf:6.2f} {h:9.1f} {v:7.1f} | {he:8.1f} {ve:7.1f} {aT:7.1f} {math.sqrt(G / beta):8.1f} {fit:6.1f} {sv:6.2f} {nu:6.1f} {mach:5.2f}")
