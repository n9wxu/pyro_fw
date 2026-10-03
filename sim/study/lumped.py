"""One lumped-parameter filter for the whole flight (HA-1, option E).

    dv/dt = a_T - g - beta * (rho(h) / rho_pad) * v * |v|

State: height above the pad, vertical speed, and two parameters.

  s        a_T = A0 * ln(1 + e^s): every upward force but drag, per unit mass
           (thrust, and the pad or the ground holding the rocket up). Written
           this way it can be large and can fall to nothing, and is never
           negative: nothing pulls a rocket down but gravity and drag.
  ln beta  drag area per unit mass, as the pad's air would give it. Never
           negative either, and it acts against the motion, so it can stop a
           climb and cannot turn it into a fall.

One equation from the pad to the ground; only the two parameters move. There
are no phases, no clamps and no special cases in the filter. The measurement
is the raw pressure, as ln p, through the standard atmosphere anchored at the
pad.
"""
import math
from est_proto import G, R_AIR, isa_T, scale_height, Site

SIGMA_FLOOR_PA = 1.2
NOISE_TAU = 2.0
A0 = 5.0  # m/s^2: the scale below which a_T bends toward zero
GAP_S = 0.25  # without a used reading for this long, the evidence starts again [SNS-PRES-11]


def std_altitude(p):
    if p >= 22632.06:
        return (288.15 / 0.0065) * (1 - (p / 101325.0) ** 0.190263)
    if p >= 5474.889:
        return 11000.0 - math.log(p / 22632.06) * R_AIR * 216.65 / G
    return 20000.0 + (216.65 / 0.001) * ((p / 5474.889) ** (-R_AIR * 0.001 / G) - 1)


def std_pressure(z):
    if z <= 11000.0:
        return 101325.0 * (1 - 0.0065 * z / 288.15) ** (G / (R_AIR * 0.0065))
    if z <= 20000.0:
        return 22632.06 * math.exp(-G * (z - 11000.0) / (R_AIR * 216.65))
    return 5474.889 * (216.65 / (216.65 + 0.001 * (z - 20000.0))) ** (G / (R_AIR * 0.001))


def softplus(s):
    return s if s > 30.0 else math.log1p(math.exp(s))


def softplus_inv(a):
    return a if a > 30.0 else math.log(math.expm1(max(a, 1e-9)))


class Lumped:
    """Tunables are the keyword arguments; the study varies them."""

    def __init__(self, k_s=1.0, k_beta=0.2, beta0=1e-3, beta_sd=2.0, beta_tau=60.0, q_v=0.25,
                 gate=6.0, run_max=2, fit_tau=1.0, inflate_cap=1e4, s_min=-3.0, thrust_tau=3.0, pad_temp_err=None, free_fall_bound=False, adapt_tau=0.2, beta_adapts=False):
        self.kS, self.kB, self.q_v = k_s, k_beta, q_v
        self.beta0, self.beta_sd, self.beta_tau = beta0, beta_sd, beta_tau
        self.gate, self.run_max = gate, run_max
        self.fit_tau, self.inflate_cap = fit_tau, inflate_cap
        self.s_min = s_min
        self.thrust_tau = thrust_tau
        self.free_fall_bound = free_fall_bound
        self.adapt_tau = adapt_tau or fit_tau   # how fast the parameters loosen; the decision check may be slower
        self.beta_adapts = beta_adapts
        self.misfit = 1.0
        self.pad_temp_err = pad_temp_err  # None: the standard atmosphere; a number: the pad's own temperature, wrong by this many kelvin
        self.true_pad_temp_c = None
        self.air = None
        self.x = None
        self.run = 0
        self.fit = 1.0  # running mean of the squared normalised innovation: about 1 while the model explains the readings
        self.nu = 0.0
        self.read_since = None  # when the present run of used readings began; a gap of GAP_S ends it
        self.used_t = None

    # The air above the pad: the standard atmosphere through the pad's
    # pressure, or the same lapse from the temperature the sensor read there.
    def pressure_at(self, h):
        return self.air.pressure(h) if self.air else std_pressure(self.pad_z + h)

    def temp_at(self, h, p):
        return self.air.temp(h) if self.air else isa_T(p)

    def start(self, p_raw, t):
        self.pad_z = std_altitude(p_raw)
        if self.pad_temp_err is not None and self.true_pad_temp_c is not None:
            self.air = Site(self.true_pad_temp_c + self.pad_temp_err, self.pad_z)
        self.rho_pad = p_raw / self.temp_at(0.0, p_raw)
        self.x = [0.0, 0.0, softplus_inv(G / A0), math.log(self.beta0)]  # at rest: the pad holds it up at 1 g
        self.P = [[0.0] * 4 for _ in range(4)]
        self.P[0][0] = (scale_height(p_raw) * SIGMA_FLOOR_PA / p_raw) ** 2
        self.P[1][1] = 0.01
        self.P[2][2] = 0.01
        self.P[3][3] = self.beta_sd ** 2
        self.t = t
        self.r_hat = SIGMA_FLOOR_PA ** 2
        self.e_prev = None

    def update(self, p_raw, t, loosen_beta=0.0):
        if self.x is None:
            if p_raw is not None:
                self.start(p_raw, t)
            return
        dt = t - self.t
        self.t = t
        if dt <= 0:
            return
        h, v, s, lb = self.x
        aT = A0 * softplus(s)
        daT_ds = A0 / (1.0 + math.exp(-s)) if s < 30.0 else A0
        p_h = self.pressure_at(h)
        T_h = self.temp_at(h, p_h)
        Hd = R_AIR * T_h / G
        k = math.exp(lb) * (p_h / T_h) / self.rho_pad

        # Predict. Drag is taken implicitly: a canopy opening at speed is stiff.
        den = 1.0 + k * abs(v) * dt
        u = v + (aT - G) * dt
        v1 = u / den
        h1 = h + v1 * dt
        g_v = max(1.0 / den - u * k * dt * (1.0 if v >= 0 else -1.0) / (den * den), 0.0)
        g_s = daT_ds * dt / den
        g_lb = -u * k * abs(v) * dt / (den * den)
        g_h = u * (k / Hd) * abs(v) * dt / (den * den)
        pull = dt / self.beta_tau
        # Thrust is brief and drag persists: left with no evidence, a_T dies
        # away in seconds and the drag keeps what it has learned.
        fade = dt / self.thrust_tau if self.thrust_tau else 0.0
        F = [[1.0 + g_h * dt, g_v * dt, g_s * dt, g_lb * dt],
             [g_h, g_v, g_s, g_lb],
             [0.0, 0.0, 1.0 - fade, 0.0],
             [0.0, 0.0, 0.0, 1.0 - pull]]
        P = self.P
        FP = [[sum(F[i][m] * P[m][j] for m in range(4)) for j in range(4)] for i in range(4)]
        P = [[sum(FP[i][m] * F[j][m] for m in range(4)) for j in range(4)] for i in range(4)]
        # The parameters loosen while the model fails to explain the readings.
        lam = min(self.inflate_cap, max(1.0, self.misfit))
        P[1][1] += self.q_v * dt
        P[2][2] += self.kS * self.kS * dt * lam
        P[3][3] += self.kB * self.kB * dt * (lam if self.beta_adapts else 1.0) + 2.0 * pull * self.beta_sd ** 2 + loosen_beta
        x = [h1, v1, s + (self.s_min - s) * fade, lb + (math.log(self.beta0) - lb) * pull]

        if self.used_t is None or t - self.used_t > GAP_S:
            self.read_since = None
        if p_raw is None:  # no reading this time: the model carries on alone
            self.x, self.P = x, P
            return

        # The reading.
        p_pred = self.pressure_at(h1)
        Hm = -G / (R_AIR * self.temp_at(h1, p_pred))
        e = math.log(p_raw) - math.log(p_pred)
        S = Hm * Hm * P[0][0] + self.r_hat / (p_raw * p_raw)
        nu = e / math.sqrt(S)
        self.nu = nu
        skipped = abs(nu) > self.gate and self.run < self.run_max
        if not skipped:  # a reading that is not used says nothing about the model
            self.misfit += min(1.0, dt / self.adapt_tau) * (min(nu * nu, 1e4) - self.misfit)
            self.fit += min(1.0, dt / self.fit_tau) * (min(nu * nu, 1e4) - self.fit)
        if skipped:
            self.run += 1  # one or two readings far off: not used
            self.e_prev = None
        else:
            if self.read_since is None:
                self.read_since = t
            self.used_t = t
            if abs(nu) <= self.gate:
                self.run = 0
                if self.e_prev is not None:
                    d = (e - self.e_prev) * p_raw
                    self.r_hat = max(SIGMA_FLOOR_PA ** 2, self.r_hat + min(1.0, dt / NOISE_TAU) * (0.5 * d * d - self.r_hat))
            self.e_prev = e
            K = [P[i][0] * Hm / S for i in range(4)]
            x = [x[i] + K[i] * e for i in range(4)]
            P = [[P[i][j] - K[i] * Hm * P[0][j] for j in range(4)] for i in range(4)]
            for i in range(4):
                for j in range(i):
                    P[i][j] = P[j][i] = 0.5 * (P[i][j] + P[j][i])
        # Nothing accelerates a rocket downward faster than gravity: a reading
        # may not take the speed below free fall from where it was, or from
        # rest if it was climbing.
        if self.free_fall_bound:
            x[1] = max(x[1], min(v, 0.0) - 2.0 * G * dt)
        x[2] = min(max(x[2], self.s_min), 400.0)
        x[3] = min(max(x[3], math.log(1e-7)), math.log(5.0))
        self.x, self.P = x, P

    # What the flight software would read.
    def h(self): return self.x[0]
    def v(self): return self.x[1]
    def a_T(self): return A0 * softplus(self.x[2])
    def beta(self): return math.exp(self.x[3])
    def sig_v(self): return math.sqrt(max(self.P[1][1], 0.0))
    def sig_h(self): return math.sqrt(max(self.P[0][0], 0.0))
    def explains(self, limit=4.0, evidence_s=1.0):
        """The model accounts for the readings, and has had a second of them to account for."""
        return self.fit < limit and self.read_since is not None and self.t - self.read_since >= evidence_s
