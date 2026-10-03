"""Descent: the speed the fire rules read. Noise against a threshold, lag on
an overspeed, and the response to a deployment."""
from est_battery import *
def pad_air_speed(e, p0):
    z, zd, _ = e.x
    p = math.exp(z); T = isa_T(p); Tp = isa_T(p0)
    v = -zd * R_AIR * T / G
    return v * math.sqrt((p / T) / (p0 / Tp))
def descend(rk, sigma, seed, drogue=25.0, main=5.0, main_agl=300.0, fail_at=None, swing=0.0, site=None):
    """Fly to apogee, open a drogue at detection, a main at main_agl.
    fail_at: height at which the drogue is lost. Returns samples of
    (t, h, true pad-air speed, estimated pad-air speed)."""
    site = site or Site(); rng = random.Random(seed)
    pl = Plant(site, rk); d = Det(1.2); n = 0; pad = 12.0; out = []
    rho0 = site.p0 / (R_AIR * site.t0); main_out = False
    while True:
        t = n * DT; n += 1; tf = t - pad
        if tf >= 0:
            for _ in range(20): pl.step(0.001)
        ps = site.pressure(pl.h)
        noise = sigma + (swing if pl.canopy_ms else 0.0)
        d.step(float(int(ps + rng.gauss(0, noise))), t)
        if d.state == 'DESCENT':
            if pl.canopy_ms is None and not getattr(pl, 'lost', False): pl.canopy_ms = drogue
            if fail_at and pl.h < fail_at and not getattr(pl, 'lost', False) and not main_out:
                pl.canopy_ms = None; pl.lost = True; pl.lost_t = tf
            if pl.h < main_agl and not main_out and not getattr(pl, 'lost', False):
                pl.canopy_ms = main; main_out = True; pl.main_t = tf
            rho = ps / (R_AIR * site.temp(pl.h))
            out.append((tf, pl.h, -pl.v * math.sqrt(rho / rho0), -pad_air_speed(d.est, math.exp(d.z0))))
        if pl.landed or tf > 900: break
    return out, pl
if __name__ == '__main__':
    print("steady drogue (25 m/s pad air): error of the estimate, by height band")
    for name in ('subsonic', 'low-drag', '20 km', '30 km'):
        for sg, sw in ((1.2, 0), (3.0, 0), (5.0, 0), (3.0, 15.0)):
            rows, pl = descend(P[name], sg, 1, swing=sw)
            bands = {}
            for tf, h, vt, ve in rows:
                if abs(vt - 25.0) > 1.0 or h < 350: continue
                bands.setdefault(int(h // 5000), []).append(ve - vt)
            txt = " ".join(f"{b*5:2d}-{b*5+5:2d}km rms {math.sqrt(sum(e*e for e in v)/len(v)):4.2f} max {max(abs(e) for e in v):4.1f}" for b, v in sorted(bands.items()))
            print(f"  {name:9s} s={sg} swing={sw:4.1f}: {txt}")
    print("drogue lost at 2 km: lag from the true speed passing 35 m/s to the estimate passing it")
    for sg in (1.2, 3.0, 5.0):
        lags = []
        for sd in range(1, 11):
            rows, pl = descend(P['low-drag'], sg, sd, fail_at=2000.0)
            tt = next(r[0] for r in rows if r[0] > pl.lost_t and r[2] > 35.0)
            te = next(r[0] for r in rows if r[0] > pl.lost_t and r[3] > 35.0)
            lags.append(te - tt)
        print(f"  s={sg}: {min(lags):.2f}..{max(lags):.2f} s")
    print("main out at 300 m (25 -> 5 m/s): time from deployment to the estimate under 10 m/s; true time")
    for sg in (1.2, 3.0, 5.0):
        ts = []
        for sd in range(1, 11):
            rows, pl = descend(P['subsonic'], sg, sd)
            tt = next(r[0] for r in rows if r[0] > pl.main_t and r[2] < 10.0) - pl.main_t
            te = next(r[0] for r in rows if r[0] > pl.main_t and r[3] < 10.0) - pl.main_t
            ts.append((te, tt))
        print(f"  s={sg}: estimate {min(t[0] for t in ts):.2f}..{max(t[0] for t in ts):.2f} s, true {ts[0][1]:.2f} s")
