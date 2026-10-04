"""Score tunings of the lumped filter over the report matrix.

    python3 sim/study/lumped_tune.py
"""
import sys
from multiprocessing import Pool
from lumped_fly import *

IN_RANGE = [n for n in ROCKETS if n != '45 km']

def cases(rockets, seeds, sigmas=(1.2, 9.0)):
    out = []
    for name in rockets:
        for sn, site in (('cold', COLD), ('hot', HOT)):
            for pn, port in PORTS.items():
                if port and name in ('hop', 'subsonic', 'mid-Mach'): continue
                for sg in sigmas:
                    for seed in range(1, seeds + 1):
                        out.append((name, sn, pn, sg, seed))
    return out

def one(arg):
    case, tune = arg
    name, sn, pn, sg, seed = case
    r = fly(ROCKETS[name], site=COLD if sn == 'cold' else HOT, sigma=sg, port=PORTS[pn], seed=seed, tune=tune)
    return case, r.get('late'), 'apogee' in r

def score(tune, rockets=IN_RANGE, seeds=2, pool=None, show=0):
    cs = cases(rockets, seeds)
    rs = pool.map(one, [(c, tune) for c in cs], chunksize=4)
    lates = [l for _, l, ok in rs if ok]
    never = sum(1 for _, _, ok in rs if not ok)
    early = [l for l in lates if l < 0]
    gross = [l for l in early if l < -0.5]
    pos = sorted(l for l in lates if l >= 0)
    clean = sorted(l for c, l, ok in rs if ok and l >= 0 and c[2] == 'clean')
    line = (f"n {len(cs)} early {len(early):3d} gross {len(gross):3d} worst {min(early, default=0):+7.2f} never {never} | "
            f"late median {pos[len(pos) // 2]:.2f} p90 {pos[int(len(pos) * 0.9)]:.2f} max {pos[-1]:.2f} | clean max {clean[-1]:.2f}")
    if show:
        for c, l, ok in sorted(rs, key=lambda r: (r[1] if r[1] is not None else 999))[:show]: print("     ", c, l)
        for c, l, ok in sorted(rs, key=lambda r: -(r[1] if r[1] is not None else 999))[:show]: print("     ", c, l)
    return line

if __name__ == '__main__':
    import json
    # python3 lumped_tune.py '{"name": {"q_v": 4.0}}' [seeds] [rockets,comma]
    variants = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {'base': {}}
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    rockets = sys.argv[3].split(',') if len(sys.argv) > 3 else IN_RANGE
    show = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    with Pool() as pool:
        for name, tune in variants.items():
            print(f"{name:16s} {score(tune, rockets=rockets, seeds=seeds, pool=pool, show=show)}", flush=True)
