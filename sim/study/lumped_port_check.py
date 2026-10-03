"""The C port of the lumped filter against this study's reference, reading for reading.

    python3 sim/study/lumped_port_check.py

Flies the study's flights, records what the reference filter was fed, replays
that through src/estimator_lumped.c (test/estimator_replay.c) and compares the
pressure and the rate wherever both explain the readings, and how often they
disagree on that. Where neither explains them (a burnout, a canopy opening)
the filter is being thrown about, a rounding difference grows, and nothing is
decided: those readings are not compared, and the two take a few seconds
after one to come back together.
"""
import math, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lumped
from lumped import Lumped, G, R_AIR
from lumped_fly import fly, ROCKETS, PORTS, COLD, HOT, ISA

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SOURCES = ['test/estimator_replay.c', 'src/estimator_table.c', 'src/estimator_constacc.c', 'src/estimator_lumped.c',
           'src/pressure_estimator.c', 'src/atmosphere.c']


class Recorded(Lumped):
    def __init__(self, **tune):
        super().__init__(**tune)
        self.fed, self.said = [], []

    def update(self, p_raw, t, loosen_beta=0.0):
        super().update(p_raw, t, loosen_beta)
        if self.x is None:
            return
        self.fed.append((round(t * 1e6), -1 if p_raw is None else int(p_raw), 1 if loosen_beta else 0))
        scale = R_AIR * self.temp_at(self.h(), self.pressure_at(self.h())) / G
        self.said.append((self.pressure_at(self.h()), -self.v() / scale, self.sig_v() / scale, self.explains()))


def build(out):
    subprocess.check_call(['cc', '-O2', '-I' + os.path.join(ROOT, 'src'), '-o', out] +
                          [os.path.join(ROOT, s) for s in SOURCES] + ['-lm'])


def compare(binary, name, site, port, seed):
    lumped_fly_Lumped = sys.modules['lumped_fly'].Lumped
    sys.modules['lumped_fly'].Lumped = Recorded
    try:
        r = fly(ROCKETS[name], site=site, sigma=3.0, port=PORTS[port], seed=seed, tune={'pad_temp_err': 0.0},
                until=None, stop_at_apogee=False, main=5.0)
    finally:
        sys.modules['lumped_fly'].Lumped = lumped_fly_Lumped
    f = r['filter']
    text = ''.join(f"{t} {p} {k}\n" for t, p, k in f.fed)
    out = subprocess.run([binary, 'lumped', str(site.t0)], input=text, capture_output=True, text=True, check=True).stdout
    worst_ln_p = 0.0
    flips = apart = compared = 0
    for (p_ref, rate_ref, sig_ref, exp_ref), line in zip(f.said, out.splitlines()):
        _, p, rate, _, sig, _, explains = line.split()
        flips += int(explains) != int(exp_ref)
        if not (exp_ref and int(explains)):
            continue
        worst_ln_p = max(worst_ln_p, abs(math.log(float(p) / p_ref)))
        compared += 1
        apart += abs(float(rate) - rate_ref) > 0.5 * sig_ref
    return len(f.said), worst_ln_p, 100.0 * apart / max(compared, 1), flips


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as d:
        binary = os.path.join(d, 'estimator_replay')
        build(binary)
        bad = 0
        print(f"{'flight':10} {'site':5} {'port':6} readings  worst ln p   rate apart by half a sigma, %  explains differs")
        for name in ROCKETS:
            for site_name, site in (('cold', COLD), ('hot', HOT)):
                for port in ('clean', 'high', 'fake'):
                    n, ln_p, apart, flips = compare(binary, name, site, port, seed=3)
                    ok = ln_p < 5e-3 and apart < 15.0 and flips <= n // 100
                    bad += not ok
                    print(f"{name:10} {site_name:5} {port:6} {n:8d}  {ln_p:10.2e}  {apart:10.2f}  {flips:6d}  {'' if ok else 'DIFFERS'}")
        sys.exit(1 if bad else 0)
