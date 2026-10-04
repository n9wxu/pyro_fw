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


PAD_READINGS = 500  # ten seconds on the pad, before any flight is thrown about


def compare(binary, name, site, port, seed):
    """Flies one flight through both. Returns what the totals are made of."""
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
    t = {'readings': len(f.said), 'flips': 0, 'compared': 0, 'apart': 0, 'worst_ln_p': 0.0, 'pad_ln_p': 0.0, 'pad_rate': 0.0}
    for i, ((p_ref, rate_ref, sig_ref, exp_ref), line) in enumerate(zip(f.said, out.splitlines())):
        _, p, rate, _, sig, _, explains = line.split()
        ln_p = abs(math.log(float(p) / p_ref))
        sigmas = abs(float(rate) - rate_ref) / max(sig_ref, 1e-9)
        if i < PAD_READINGS:
            t['pad_ln_p'] = max(t['pad_ln_p'], ln_p)
            t['pad_rate'] = max(t['pad_rate'], sigmas)
        t['flips'] += int(explains) != int(exp_ref)
        if not (exp_ref and int(explains)):
            continue
        t['worst_ln_p'] = max(t['worst_ln_p'], ln_p)
        t['compared'] += 1
        t['apart'] += sigmas > 0.5
    return t


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as d:
        binary = os.path.join(d, 'estimator_replay')
        build(binary)
        total = {'readings': 0, 'flips': 0, 'compared': 0, 'apart': 0}
        worst_ln_p = pad_ln_p = pad_rate = 0.0
        print(f"{'flight':10} {'site':5} {'port':6} readings  worst ln p  rate apart %  explains differs")
        for name in ROCKETS:
            for site_name, site in (('cold', COLD), ('hot', HOT)):
                for port in ('clean', 'high', 'fake'):
                    t = compare(binary, name, site, port, seed=3)
                    for k in total:
                        total[k] += t[k]
                    worst_ln_p = max(worst_ln_p, t['worst_ln_p'])
                    pad_ln_p, pad_rate = max(pad_ln_p, t['pad_ln_p']), max(pad_rate, t['pad_rate'])
                    print(f"{name:10} {site_name:5} {port:6} {t['readings']:8d}  {t['worst_ln_p']:10.2e}  "
                          f"{100.0 * t['apart'] / max(t['compared'], 1):10.2f}  {t['flips']:6d}")
        apart = 100.0 * total['apart'] / max(total['compared'], 1)
        flips = 100.0 * total['flips'] / total['readings']
        print(f"\non the pad, before any flight: ln p within {pad_ln_p:.1e}, rate within {pad_rate:.3f} sigma")
        print(f"over {total['readings']} readings: where both explain them, {apart:.2f} % have rates half a sigma apart, "
              f"ln p within {worst_ln_p:.1e}; {flips:.2f} % disagree on explaining")
        # The same arithmetic, so on the pad they agree closely. In flight a
        # rounding difference grows at each burnout and canopy, differently
        # on each compiler and library: the totals are held, not each flight.
        ok = pad_ln_p < 5e-5 and pad_rate < 0.1 and apart < 5.0 and flips < 2.0 and worst_ln_p < 2e-2
        print("PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)
