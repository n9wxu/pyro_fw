"""Raw pressure into one Kalman filter: pad tracking and liftoff detection.
State [p, pdot, pddot], white-jerk process, steady-state gain.
Launch rule (FLT-LAUNCH-07 in pressure): p_ref - p > 366 Pa (100 ft at sea
level) and -pdot > 60 Pa/s (5 m/s), held 100 ms."""
import math, random, sys
DT = 0.02
RHO_G = 12.01          # Pa per metre at a sea-level pad
H_THR = 30.48 * RHO_G  # 366 Pa
V_THR = 5.0 * RHO_G    # 60 Pa/s
HOLD = 5               # samples = 100 ms
REF_DELAY = int(3.0 / DT)

def gain(q, r):
    """Steady-state Kalman gain by iterating the Riccati recursion."""
    dt = DT
    F = [[1, dt, dt*dt/2], [0, 1, dt], [0, 0, 1]]
    Q = [[q*dt**5/20, q*dt**4/8, q*dt**3/6],
         [q*dt**4/8,  q*dt**3/3, q*dt**2/2],
         [q*dt**3/6,  q*dt**2/2, q*dt]]
    P = [[r, 0, 0], [0, r, 0], [0, 0, r]]
    def mm(A, B): return [[sum(A[i][k]*B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    Ft = [[F[j][i] for j in range(3)] for i in range(3)]
    K = [0, 0, 0]; S = r
    for _ in range(20000):
        P = mm(mm(F, P), Ft)
        P = [[P[i][j] + Q[i][j] for j in range(3)] for i in range(3)]
        S = P[0][0] + r
        Kn = [P[i][0] / S for i in range(3)]
        P = [[P[i][j] - Kn[i]*P[0][j] for j in range(3)] for i in range(3)]
        if max(abs(Kn[i]-K[i]) for i in range(3)) < 1e-12 * max(1, abs(Kn[2])):
            K = Kn; break
        K = Kn
    return K, S

class Det:
    def __init__(self, q, sigma, mode):
        self.K, self.S = gain(q, sigma*sigma)
        self.mode = mode                 # 'raw', 'clip', 'median'
        self.clip = 6.0 * math.sqrt(self.S)
        self.x = None; self.hist = []; self.raw = []
        self.hold = 0; self.launched = False
        self.ref = None; self.t0 = None; self.n = 0
    def step(self, z):
        self.n += 1
        if self.mode == 'median':
            self.raw.append(z); self.raw = self.raw[-3:]
            z = sorted(self.raw)[len(self.raw)//2]
        if self.x is None:
            self.x = [z, 0.0, 0.0]
        p, v, a = self.x
        p += v*DT + a*DT*DT/2; v += a*DT
        e = z - p
        if self.mode == 'clip':
            e = max(-self.clip, min(self.clip, e))
        p += self.K[0]*e; v += self.K[1]*e; a += self.K[2]*e
        self.x = [p, v, a]
        if self.launched: return
        self.hist.append(p)
        if len(self.hist) > REF_DELAY + 1: self.hist.pop(0)
        ref = self.hist[0]
        if ref - p > H_THR and -v > V_THR:
            self.hold += 1
            if self.hold >= HOLD:
                self.launched = True; self.ref = ref
                # liftoff back-solved from the state: from rest, t = 2h/v
                self.t0 = self.n*DT - 2.0*(ref - p)/max(-v, 1e-9)
        else:
            self.hold = 0

def pad_noise(rng, sigma, gust_rms, n):
    """White sensor noise + weather ramp + Gauss-Markov gusts (tau 3 s)."""
    tau = 3.0; a = math.exp(-DT/tau); s = gust_rms*math.sqrt(1-a*a); g = 0.0
    for i in range(n):
        g = a*g + s*rng.gauss(0, 1)
        yield 101325.0 + 0.03*i*DT + g + rng.gauss(0, sigma)

def false_launches(q, sigma, mode, gust, hours, seed):
    rng = random.Random(seed); d = Det(q, sigma, mode); count = 0
    worst_v = 0.0
    for z in pad_noise(rng, sigma, gust, int(hours*3600/DT)):
        d.step(z)
        if d.n > 500: worst_v = max(worst_v, abs(d.x[1]))
        if d.launched:
            count += 1; d.launched = False; d.hold = 0
    return count, worst_v

def glitch(q, sigma, mode, G, run_len, seed):
    rng = random.Random(seed); d = Det(q, sigma, mode)
    n = 1500
    for i in range(n):
        z = 101325.0 + rng.gauss(0, sigma)
        if 1000 <= i < 1000 + run_len: z -= G
        d.step(z)
        if d.launched: return True
    return False

def launch(q, sigma, mode, g_net, seed):
    rng = random.Random(seed); d = Det(q, sigma, mode)
    acc = g_net * 9.80665; pre = 500  # 10 s of pad
    p0 = 101325.0
    for i in range(pre + 400):
        t = (i - pre) * DT
        h = 0.5*acc*t*t if t > 0 else 0.0
        z = p0*math.exp(-h/8434.0) + rng.gauss(0, sigma)
        d.step(z)
        if d.launched:
            t_ideal = math.sqrt(2*30.48/acc)
            return t - t_ideal, h, (d.t0 - pre*DT), d.ref - p0
    return None

if __name__ == '__main__':
    QS = [1e3, 1e5, 1e7]
    print("gains  (sigma 3 Pa)")
    for q in QS:
        K, S = gain(q, 9.0)
        print(f"  q={q:.0e}  Kp={K[0]:.3f}  Kv={K[1]:.2f}/s  Ka={K[2]:.1f}/s2")
    print("\npad, 2 h, gusts 30 Pa rms: false launches, worst |pdot| in m/s")
    for q in QS:
        for s in (1.2, 3.0, 5.0):
            c, w = false_launches(q, s, 'raw', 30.0, 2.0, 1)
            print(f"  q={q:.0e} sigma={s}: {c} false, worst {w/RHO_G:.1f} m/s")
    print("\nsmallest bad reading (Pa low) that declares a launch, sigma 3")
    GS = [300, 1000, 3000, 6000, 12000, 30000, 60000]
    for run_len in (1, 2):
        for mode in ('raw', 'clip', 'median'):
            for q in QS:
                hit = next((G for G in GS if any(glitch(q, 3.0, mode, G, run_len, sd) for sd in range(5))), None)
                print(f"  {run_len} reading(s) {mode:6s} q={q:.0e}: {hit if hit else 'none up to 60 kPa'}")
    print("\nlaunch, sigma 3, 100 seeds: delay after true 100 ft (ms) mean/max, height at detect (m), T+0 error (ms) mean/worst, ref error (Pa) worst")
    for mode in ('raw', 'clip', 'median'):
        for q in QS:
            for g in (1.5, 3, 10, 30, 100):
                res = [launch(q, 3.0, mode, g, sd) for sd in range(100)]
                ok = [r for r in res if r]
                if len(ok) < 100:
                    print(f"  {mode:6s} q={q:.0e} {g:5}g: MISSED {100-len(ok)}"); continue
                dl = [r[0]*1000 for r in ok]; hh = [r[1] for r in ok]
                t0 = [r[2]*1000 for r in ok]; rf = [abs(r[3]) for r in ok]
                print(f"  {mode:6s} q={q:.0e} {g:5}g: delay {sum(dl)/100:5.0f}/{max(dl):5.0f}  h {sum(hh)/100:6.1f}  T0 {sum(t0)/100:6.0f}/{max(t0,key=abs):6.0f}  ref {max(rf):5.1f}")
