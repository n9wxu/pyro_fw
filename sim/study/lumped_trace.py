"""Trace one flight: python3 lumped_trace.py rocket site port sigma seed '{"tune":..}' t0 t1 step"""
import sys, json, math
from lumped_fly import *
name, sn, pn, sg, seed = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), int(sys.argv[5])
tune = json.loads(sys.argv[6]) if len(sys.argv) > 6 else {}
t0, t1, every = float(sys.argv[7]), float(sys.argv[8]), int(sys.argv[9])
tr = []
r = fly(ROCKETS[name], site=COLD if sn == 'cold' else HOT, sigma=sg, port=PORTS[pn], seed=seed, tune=tune, trace=tr, until=t1 + 1, stop_at_apogee=False)
print({k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items() if k != 'filter'})
print("     t    h_true  v_true |    h_est   v_est     a_T   v_term     fit  sig_v     nu  mach")
for i, row in enumerate(tr):
    tf, h, v, he, ve, aT, beta, fit, sv, nu, mach = row
    if t0 <= tf <= t1 and i % every == 0:
        print(f"{tf:6.2f} {h:9.1f} {v:7.1f} | {he:8.1f} {ve:7.1f} {aT:7.1f} {math.sqrt(G / beta):8.1f} {fit:7.1f} {sv:6.2f} {nu:6.1f} {mach:5.2f}")
