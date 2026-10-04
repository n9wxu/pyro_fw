from est_fly import *
P = {
    'subsonic': Rocket(1.0, 0.10, 141.0, 1.5, 0.0020, 2.0),
    'mid-Mach': Rocket(1.0, 0.15, 220.0, 1.6, 0.0020, 2.0),
    'draggy':   Rocket(2.0, 0.60, 1709.0, 1.5, 0.0045, 3.0),
    'low-drag': Rocket(8.0, 4.00, 1216.0, 5.0, 0.0008, 1.5),
    '30 g':     Rocket(1.0, 0.30, 395.0, 1.1, 0.0015, 2.0),
}
# find rockets for 15, 20, 30 km
def apogee(rk, site=Site()):
    pl = Plant(site, rk)
    while not pl.apogee and pl.t < 600: pl.step(0.002)
    return pl.apogee_h, pl.max_mach, pl.apogee_t
for name, rk in (('15 km', Rocket(8.0, 2.0, 1000.0, 7.0, 0.0008, 1.5)), ('20 km', Rocket(8.0, 3.0, 1700.0, 5.0, 0.0008, 1.5)), ('30 km', Rocket(8.0, 6.0, 2000.0, 6.0, 0.0008, 1.5)), ('45 km', Rocket(8.0, 8.0, 2100.0, 7.5, 0.0008, 1.5))):
    P[name] = rk
if __name__ == '__main__':
    for n in ('15 km', '20 km', '30 km'):
        print(n, "apogee %.0f m Mach %.2f at %.1f s" % apogee(P[n]))
    HIGH = (+1.0, 0.02, 0.05, 0.03); LOW = (-1.0, 0.02, 0.05, 0.03); FAKE = (+1.0, 0.06, 0.14, 0.05)
    COLD, HOT, ISA = Site(10, 0), Site(45, 2000), Site(15, 0)
    def batt(label, rk, n=20, **kw):
        rs = [fly(rk, seed=s, **kw) for s in range(1, n + 1)]
        def rng(k):
            v = [r[k] for r in rs if r.get(k) is not None]
            return ("%6.2f..%6.2f" % (min(v), max(v))) if v else "   -none-    "
        early = sum(1 for r in rs if r.get('early'))
        noapo = sum(1 for r in rs if 'apogee' not in r)
        fb = sum(1 for r in rs if r.get('fallback'))
        norel = sum(1 for r in rs if 'flag' in r and 'release' not in r)
        print(f"{label:34s} flagM {rng('flag_mach')} relM {rng('release_mach')} rel_t {rng('release')} late {rng('apo_late')} below {rng('apo_h_below')} | early {early} none {noapo} fallback {fb} unreleased {norel} apo_t {rs[0]['true_apogee_t']:.0f}")
    import sys
    if len(sys.argv) < 2:
        for site, sn in ((COLD, 'cold'), (HOT, 'hot')):
            for name in ('subsonic', 'mid-Mach', 'draggy', 'low-drag', '30 g'):
                for port, pn in ((None, 'clean'), (HIGH, 'high'), (LOW, 'low'), (FAKE, 'fake')):
                    if port and name in ('subsonic', 'mid-Mach'): continue
                    batt(f"{name} {sn} {pn} s=3", P[name], n=6, site=site, sigma=3.0, port=port, sigma_assumed=1.2)
    
    for name in ('15 km', '20 km', '30 km'):
        for sg in (1.2, 3.0, 5.0):
            batt(f"{name} s={sg}", P[name], n=10, sigma=sg, sigma_assumed=1.2)
