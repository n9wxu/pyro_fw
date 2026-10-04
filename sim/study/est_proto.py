"""Prototype of the DD-085 estimator: a Kalman filter on raw pressure in
log-pressure, with its correction limited and its process and measurement
noise tracked from the innovations. Flown against a port of sim/mach_plant.c
extended with the 1976 atmosphere's third layer.

    python3 sim/study/est_proto.py            the scenario table
"""
import math, random

G, R_AIR, GAMMA = 9.80665, 287.05, 1.4

# ── atmosphere: pad at (temp_c, elev_m); lapse to 11 km, isothermal to 20 km,
#    +1 K/km above, as the standard's layers, shifted by the pad's offset ──
class Site:
    def __init__(self, temp_c=15.0, elev_m=0.0):
        self.t0 = temp_c + 273.15
        self.elev = elev_m
        self.p0 = 101325.0 * (1 - 0.0065 * elev_m / 288.15) ** (G / (R_AIR * 0.0065))
        self.tt = self.t0 - 0.0065 * (11000.0 - elev_m)
        self.pt = self.p0 * (self.tt / self.t0) ** (G / (R_AIR * 0.0065))
        self.p20 = self.pt * math.exp(-G * 9000.0 / (R_AIR * self.tt))
    def temp(self, h):
        z = self.elev + h
        if z <= 11000.0: return self.t0 - 0.0065 * (z - self.elev)
        if z <= 20000.0: return self.tt
        return self.tt + 0.001 * (z - 20000.0)
    def pressure(self, h):
        z = self.elev + h
        if z <= 11000.0:
            return self.p0 * (self.temp(h) / self.t0) ** (G / (R_AIR * 0.0065))
        if z <= 20000.0:
            return self.pt * math.exp(-G * (z - 11000.0) / (R_AIR * self.tt))
        return self.p20 * (self.tt / self.temp(h)) ** (G / (R_AIR * 0.001))
    def sound(self, h): return math.sqrt(GAMMA * R_AIR * self.temp(h))

class Rocket:
    def __init__(self, dry, prop, thrust, burn, cda, wave, canopy=20.0):
        self.dry, self.prop, self.thrust, self.burn, self.cda, self.wave, self.canopy = dry, prop, thrust, burn, cda, wave, canopy

class Plant:
    def __init__(self, site, rocket):
        self.s, self.r = site, rocket
        self.t = self.h = self.v = self.mach = self.max_mach = 0.0
        self.canopy_ms = None       # terminal rate in pad air once a canopy is out
        self.apogee = False; self.apogee_t = self.apogee_h = 0.0
        self.landed = False; self.a = 0.0
    def step(self, dt):
        if self.landed: return
        s, r = self.s, self.r
        rho = s.pressure(self.h) / (R_AIR * s.temp(self.h))
        if self.canopy_ms and self.v < 0:
            rho0 = s.p0 / (R_AIR * s.t0)
            k = G / (self.canopy_ms ** 2) * rho / rho0
            a = -G + k * self.v * self.v
        else:
            burning = self.t < r.burn
            mass = r.dry + (r.prop * (1 - self.t / r.burn) if burning else 0.0)
            d = (abs(self.mach) - 1.05) / 0.25
            drag = 0.5 * rho * self.v * abs(self.v) * r.cda * (1 + r.wave * math.exp(-d * d))
            a = ((r.thrust if burning else 0.0) - drag) / mass - G
        vb = self.v
        self.a = a
        self.v += a * dt; self.h += self.v * dt; self.t += dt
        self.mach = self.v / s.sound(self.h)
        self.max_mach = max(self.max_mach, abs(self.mach))
        if not self.apogee and self.t > r.burn and vb > 0 >= self.v:
            self.apogee, self.apogee_t, self.apogee_h = True, self.t, self.h
        if self.h <= 0 and self.t > r.burn:
            self.h = self.v = self.mach = 0.0; self.landed = True

def port_error(port, mach, static_pa):
    if port is None: return 0.0
    sign, below, above, slope = port
    m = abs(mach)
    if m < 0.85: return 0.0
    c = below * (m - 0.85) / 0.15 if m < 1.0 else above + slope * (m - 1.0)
    return sign * c * 0.5 * GAMMA * static_pa * m * m

# ── the estimator ──────────────────────────────────────────────────────
H_REF = 8000.0
Q_JERK = 7.0 / H_REF ** 2       # white-jerk PSD, m^2/s^5 over H^2
OUTLIER = 6.0                   # a reading this many sigma off is not used...
OUTLIER_RUN = 2                 # ...unless it is the third in a row: then it is data
SIGMA_FLOOR_PA = 1.2            # the quietest sensor fitted
NOISE_TAU = 2.0                 # the measurement noise is tracked over this long

class Est:
    def __init__(self, sigma_pa=3.0, q=Q_JERK):
        self.sigma = sigma_pa
        self.q = q
        self.x = None
        self.P = None
        self.t = None
        self.run = 0
        self.b = 0.0; self.rs = 1.0
        self.r_hat = None; self.e_prev = None
    def update(self, p_raw, t):
        y = math.log(p_raw)
        if self.x is None:
            self.x = [y, 0.0, 0.0]
            r0 = (max(self.sigma, SIGMA_FLOOR_PA) / p_raw) ** 2
            self.P = [[r0, 0, 0], [0, 1e-6, 0], [0, 0, 1e-6]]
            self.t = t
            return
        dt = t - self.t; self.t = t
        if dt <= 0: return
        z, v, a = self.x
        z += v * dt + 0.5 * a * dt * dt; v += a * dt
        P = self.P; q = self.q
        F = ((1, dt, 0.5 * dt * dt), (0, 1, dt), (0, 0, 1))
        FP = [[sum(F[i][k] * P[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
        P = [[sum(FP[i][k] * F[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
        d2, d3, d4, d5 = dt * dt, dt ** 3, dt ** 4, dt ** 5
        Q = ((q * d5 / 20, q * d4 / 8, q * d3 / 6), (q * d4 / 8, q * d3 / 3, q * d2 / 2), (q * d3 / 6, q * d2 / 2, q * dt))
        for i in range(3):
            for j in range(3): P[i][j] += Q[i][j]
        if self.r_hat is None: self.r_hat = max(self.sigma, SIGMA_FLOOR_PA) ** 2
        Rm = self.r_hat / p_raw ** 2
        S = P[0][0] + Rm
        e = y - z
        nu = e / math.sqrt(S)
        self.nu = nu
        if abs(nu) > OUTLIER and self.run < OUTLIER_RUN:
            self.run += 1                      # predicted only
            self.e_prev = None
        else:
            # the noise is the scatter between successive innovations, which a
            # model running behind does not change
            if self.e_prev is not None and abs(nu) <= OUTLIER:
                d = (e - self.e_prev) * p_raw
                self.r_hat = max(SIGMA_FLOOR_PA ** 2, self.r_hat + min(1.0, dt / NOISE_TAU) * (0.5 * d * d - self.r_hat))
            self.e_prev = e if abs(nu) <= OUTLIER or True else None
            if abs(nu) <= OUTLIER: self.run = 0
            K = [P[i][0] / S for i in range(3)]
            z += K[0] * e; v += K[1] * e; a += K[2] * e
            P = [[P[i][j] - K[i] * P[0][j] for j in range(3)] for i in range(3)]
            for i in range(3):
                for j in range(i):
                    P[i][j] = P[j][i] = 0.5 * (P[i][j] + P[j][i])
        self.x = [z, v, a]; self.P = P
    def p(self): return math.exp(self.x[0])
    def sig_v(self): return math.sqrt(max(self.P[1][1], 0.0))
    def sigma_pa(self): return math.sqrt(self.r_hat) if self.r_hat else max(self.sigma, SIGMA_FLOOR_PA)
    def sig_a(self): return math.sqrt(max(self.P[2][2], 0.0))

def isa_T(p):
    if p >= 22632.06: return 288.15 * (p / 101325.0) ** 0.190263
    if p >= 5474.889: return 216.65
    return 216.65 * (max(p, 868.0) / 5474.889) ** -0.0292716

def scale_height(p): return R_AIR * isa_T(p) / G

if __name__ == '__main__':
    import sys
    PROFILES = {
        'subsonic': Rocket(1.0, 0.10, 141.0, 1.5, 0.0020, 2.0),
        'mid-Mach': Rocket(1.0, 0.15, 220.0, 1.6, 0.0020, 2.0),
        'draggy':   Rocket(2.0, 0.60, 1709.0, 1.5, 0.0045, 3.0),
        'low-drag': Rocket(8.0, 4.00, 1216.0, 5.0, 0.0008, 1.5),
        '30 g':     Rocket(1.0, 0.30, 395.0, 1.1, 0.0015, 2.0),
        '30 km':    Rocket(8.0, 14.0, 2600.0, 12.0, 0.0006, 1.5),
    }
    for name, rk in PROFILES.items():
        pl = Plant(Site(), rk)
        while not pl.apogee and pl.t < 300: pl.step(0.001)
        print(f"{name:9s} Mach {pl.max_mach:.2f} apogee {pl.apogee_h:7.0f} m at {pl.apogee_t:5.1f} s")
