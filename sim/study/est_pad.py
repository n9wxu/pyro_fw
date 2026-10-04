"""The pad: false launches from noise, gusts and bad readings; launch delay."""
from est_fly import *
def pad(hours, sigma, gust, seed, glitch=None):
    rng = random.Random(seed); d = Det(1.2); g = 0.0
    a = math.exp(-DT / 3.0); s = gust * math.sqrt(1 - a * a)
    n = int(hours * 3600 / DT); launches = 0; vmax = 0.0
    for i in range(n):
        g = a * g + s * rng.gauss(0, 1)
        p = 101325.0 + 0.03 * i * DT + g + rng.gauss(0, sigma)
        if glitch and glitch[0] <= i < glitch[0] + glitch[2]: p -= glitch[1]
        d.step(float(int(p)), i * DT)
        if d.est.x and i > 500:
            vmax = max(vmax, abs(d.est.x[1]) * 8434)
        if d.state != 'PAD':
            launches += 1; d.state = 'PAD'; d.hold = None; d.flag = False
    return launches, vmax, d
if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1:
        print("2 h on the pad: false launches, worst |speed| m/s")
        for sg in (1.2, 3.0, 5.0):
            for gust in (0.0, 30.0):
                l, v, _ = pad(2.0, sg, gust, 1)
                print(f"  sigma {sg} gust {gust:4.1f}: {l} launches, worst {v:.1f} m/s")
        print("consecutive bad readings needed to declare a launch (each this far low)")
        for GL in (300, 1000, 3000, 12000, 60000):
            need = None
            for run in range(1, 40):
                if any(pad(40 / 3600.0, 3.0, 0.0, sd, glitch=(1500, GL, run))[0] for sd in range(1, 4)):
                    need = run; break
            print(f"  {GL:6d} Pa: {need if need else 'none up to 39'}" + (f" ({need * 20} ms)" if need else ""))
    
    print("launch at constant net g, sigma 3: detection after the true 100 ft (ms) and T+0 error (ms), 30 seeds")
    for g in (1.5, 3, 10, 30, 66, 100):
        dl = []; t0 = []; hh = []
        for sd in range(1, 31):
            rng = random.Random(sd); d = Det(1.2); acc = g * G
            for i in range(2000):
                t = i * DT; tf = t - 12.0
                h = 0.5 * acc * tf * tf if tf > 0 else 0.0
                d.step(float(int(101325.0 * math.exp(-h / 8434.0) + rng.gauss(0, 3.0))), t)
                if d.state != 'PAD':
                    dl.append((tf - math.sqrt(2 * 30.48 / acc)) * 1000); t0.append((d.t0 - 12.0) * 1000); hh.append(h); break
        print(f"  {g:5}g: {len(dl)}/30 detected, delay {sum(dl)/len(dl):5.0f} (max {max(dl):4.0f}) ms at {sum(hh)/len(hh):5.0f} m; T+0 {sum(t0)/len(t0):+5.0f} (worst {max(t0, key=abs):+5.0f}) ms")
