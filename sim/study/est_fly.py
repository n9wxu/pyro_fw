"""Fly the estimator prototype with the detectors it must serve."""
import math, random, sys
from est_proto import *

DT = 0.02
LN_APO = 1e-4
def h_to_dz(h, p0):
    """ln(p/p0) at height h above a pad at p0, standard atmosphere anchored at the pad."""
    t_pad = isa_T(p0)
    return (G / (R_AIR * 0.0065)) * math.log(max(1e-6, 1 - 0.0065 * h / t_pad))

class Det:
    def __init__(self, sigma_assumed=3.0):
        self.est = Est(sigma_assumed)
        self.state = 'PAD'
        self.hist = []          # (t, z_est, y_raw) on the pad
        self.z0 = None
        self.t0 = None
        self.launch_t = None; self.hold = None
        self.flag = False; self.flag_t = None; self.released = False; self.release_t = None
        self.rel_since = None; self.fb_since = None; self.back_since = None; self.fallback = False
        self.apo_since = None; self.apogee_t = None
        self.armed = False; self.vmax = 0.0; self.arm_height = False
        self.zmin = None
        self.thrust = False; self.burnout_t = None
        self.rise_t = None
    def held(self, cond, name, t, hold):
        s = getattr(self, name)
        if not cond:
            setattr(self, name, None); return False
        if s is None:
            setattr(self, name, t); s = t
        return t - s >= hold
    def step(self, p_raw, t):
        e = self.est
        e.update(p_raw, t)
        y_now = math.log(p_raw)
        self.ys = (getattr(self, 'ys', []) + [(t, y_now)])[-3:]
        self.short = (self.ys[-1][1] - self.ys[0][1]) / (self.ys[-1][0] - self.ys[0][0]) if len(self.ys) == 3 else 0.0
        if e.P is None or e.t is None: return
        z, zd, zdd = e.x
        p = math.exp(z)
        H = scale_height(p)
        if abs(getattr(e, 'nu', 0.0)) > 4.0: self.rough_t = t
        smooth = t - getattr(self, 'rough_t', -9.0) >= 1.0   # no reading far off the estimate for a second
        v = -zd * H                      # m/s up
        if self.state == 'PAD':
            y = math.log(p_raw)
            self.hist.append((t, z, y))
            if len(self.hist) > 400: self.hist.pop(0)
            if self.z0 is None:
                if len(self.hist) >= 50: self.z0 = sum(h[1] for h in self.hist) / len(self.hist)
                return
            # the reference: mean of the estimate over the last 5 s, gated at 50 Pa
            if abs(math.exp(z) - math.exp(self.z0)) < 50.0:
                rec = [h[1] for h in self.hist if t - h[0] <= 5.0]
                self.z0 = sum(rec) / len(rec)
            p0 = math.exp(self.z0)
            rise = y - self.z0 < h_to_dz(0.5, p0)
            if not rise: self.rise_t = None
            elif self.rise_t is None: self.rise_t = t
            if self.rise_t is not None and not self.flag and (-zd > 0.029 or -self.short > 0.029):
                self.flag = True; self.flag_t = t; self.flag_z = z
            alt_ok = z - self.z0 < h_to_dz(30.48, p0)
            if self.held(alt_ok and v > 5.0, 'hold', t, 0.1):
                self.state = 'ASCENT'; self.launch_t = t
                self.t0 = self.rise_t if self.rise_t is not None else t
                pre = [h[1] for h in self.hist if self.t0 - 5.0 <= h[0] <= self.t0 - 0.25]
                if pre: self.z0 = sum(pre) / len(pre)
                self.thrust = True
            return
        if self.state == 'ASCENT':
            self.vmax = max(self.vmax, v)
            a_up = -(zdd) * H
            was = self.thrust
            self.thrust = a_up > 0
            if was and not self.thrust and self.burnout_t is None: self.burnout_t = t
            if z - self.z0 < math.log(0.9965): self.arm_height = True
            sv = e.sig_v()
            if not self.flag:
                if -zd > 0.029 or (not self.released and -self.short > 0.029):
                    self.flag = True; self.flag_t = t; self.flag_z = z
            else:
                coasting = smooth and 0 < -zd < 0.022 and (zdd + zd * zd) >= 0.0009
                if self.held(coasting, 'rel_since', t, 1.0):
                    self.flag = False; self.released = True; self.release_t = t; self.zmin = z
                else:
                    falling = smooth and 0 < zd < 0.022 and (zdd + zd * zd) >= 0.0009
                    back = smooth and zd > 0 and z > self.flag_z
                    if self.held(falling, 'fb_since', t, 2.0) or self.held(back, 'back_since', t, 0.0):
                        self.fallback = True; self.armed = True
                        self.state = 'DESCENT'; self.apogee_t = t
                        return
            if not self.flag:
                if self.zmin is None or z < self.zmin: self.zmin = z
            if not self.armed and self.arm_height and self.vmax >= 10 and v < 10:
                self.armed = True
            if self.armed and not self.flag and self.held(zd > 3 * sv, 'apo_since', t, 0.06):
                self.state = 'DESCENT'; self.apogee_t = t

def fly(rk, site=None, sigma=3.0, sigma_assumed=None, port=None, seed=1, pad_s=12.0, canopy=None, until=None, glitch=None, trace=None):
    site = site or Site()
    rng = random.Random(seed)
    pl = Plant(site, rk)
    d = Det(sigma_assumed or sigma)
    res = {}
    t = 0.0; n = 0
    tb = None
    while True:
        t = n * DT; n += 1
        tf = t - pad_s
        if tf >= 0:
            for _ in range(20): pl.step(0.001)
        ps = site.pressure(pl.h)
        sensed = ps + port_error(port, pl.mach, ps) + rng.gauss(0, sigma)
        if glitch and glitch[0] <= tf < glitch[0] + glitch[2] * DT: sensed += glitch[1]
        d.step(float(int(sensed)), t)
        if d.state != 'PAD' and 'launch' not in res:
            res['launch'] = tf; res['launch_h'] = pl.h; res['t0_err'] = (d.t0 - pad_s)
        if d.flag and 'flag' not in res:
            res['flag'] = tf; res['flag_mach'] = pl.mach
        if d.released and 'release' not in res:
            res['release'] = tf; res['release_mach'] = pl.mach
        if d.burnout_t and 'burnout' not in res: res['burnout'] = tf - rk.burn
        if d.state == 'DESCENT' and 'apogee' not in res:
            res['apogee'] = tf; res['fallback'] = d.fallback; res['armed'] = d.armed
            if canopy: pl.canopy_ms = canopy
            res['apo_late'] = tf - pl.apogee_t if pl.apogee else None
            res['apo_h_below'] = (pl.apogee_h - pl.h) if pl.apogee else None
            res['early'] = not pl.apogee
            if until is None: break
        if trace is not None and d.est.x is not None:
            z, zd, zdd = d.est.x; H = scale_height(math.exp(z))
            trace.append((tf, pl.h, pl.v, pl.a, -zd * H, -zdd * H, d.est.q, d.est.rs, d.est.b, d.est.sig_v() * H))
        if pl.landed or tf > (until or 400): break
    res['true_apogee_t'] = pl.apogee_t; res['true_apogee_h'] = pl.apogee_h; res['max_mach'] = pl.max_mach
    return res

if __name__ == '__main__':
    PROFILES = {
        'subsonic': Rocket(1.0, 0.10, 141.0, 1.5, 0.0020, 2.0),
        'mid-Mach': Rocket(1.0, 0.15, 220.0, 1.6, 0.0020, 2.0),
        'draggy':   Rocket(2.0, 0.60, 1709.0, 1.5, 0.0045, 3.0),
        'low-drag': Rocket(8.0, 4.00, 1216.0, 5.0, 0.0008, 1.5),
        '30 g':     Rocket(1.0, 0.30, 395.0, 1.1, 0.0015, 2.0),
    }
    def show(name, r):
        ks = ['launch', 'launch_h', 't0_err', 'flag', 'flag_mach', 'release', 'release_mach', 'burnout', 'apo_late', 'apo_h_below', 'fallback', 'early']
        print(f"{name:22s}", " ".join(f"{k}={r[k]:.2f}" if isinstance(r.get(k), float) else f"{k}={r.get(k)}" for k in ks if k in r))
    for name, rk in PROFILES.items():
        for sg in (1.2, 3.0):
            show(f"{name} s={sg}", fly(rk, sigma=sg))
