/*
 * "lumped": one equation of motion from the pad to the ground [DD-092].
 *
 *     dv/dt = a_T - g - beta (rho / rho_pad) v |v|
 *
 * An extended Kalman filter on height above the pad, vertical speed and two
 * parameters it learns from the readings: s, with a_T = A0 ln(1 + e^s), and
 * ln(beta). Derived, tuned and measured in docs/lumped_parameter_filter.md;
 * sim/study/lumped.py is the reference this is checked against.
 *
 * The air is the standard atmosphere's layers through the pad's pressure and
 * the sensor's temperature there.
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimator.h"
#include "atmosphere.h"
#include "pressure_estimator.h"
#include <math.h>
#include <string.h>

#define G0 9.80665f
#define R_AIR 287.05287f
#define N 4
enum { H, V, S, LB };

#define A0 5.0f      /* m/s^2: below this a_T bends toward zero */
#define S_NOISE 1.0f /* random walk of s, per root second */
#define S_MIN -3.0f
#define S_MAX 400.0f
#define THRUST_TAU_S 3.0f  /* with no evidence a_T dies away; drag keeps what it learned */
#define BETA_PRIOR 1e-3f   /* 1/m */
#define BETA_PRIOR_SD 2.0f /* of ln(beta) */
#define BETA_TAU_S 60.0f   /* the pull back to the prior */
#define BETA_NOISE 0.2f
#define LN_BETA_MIN -16.118f /* ln 1e-7 */
#define LN_BETA_MAX 1.6094f  /* ln 5 */
#define SPEED_NOISE 0.25f    /* (m/s)^2 per second */
#define GATE_SIGMAS 6.0f
#define OUTLIER_RUN_MAX 2
#define FIT_TAU_S 1.0f
#define FIT_LIMIT 4.0f
#define ADAPT_TAU_S 0.2f
#define LOOSEN_MAX 1e4f
#define SQUARED_MAX 1e4f
#define NOISE_TAU_S 2.0f
#define EVIDENCE_US 1000000u
#define GAP_US 250000u /* [SNS-PRES-11] */
#define STEP_MAX_S 0.1f
#define PULSE_BLANK_US 1000000u /* a charge pressurises the bay */
#define PULSE_LOOSENS 4.0f      /* variance added to ln(beta): something has deployed */
#define TROPOPAUSE_M 11000.0f
#define STRATOSPHERE_M 20000.0f
#define LAPSE 0.0065f
#define STRATO_LAPSE 0.001f

typedef struct {
    float pad_m, pad_k, pad_pa; /* the pad's standard altitude, and the air there */
    float tropopause_k, tropopause_pa, stratosphere_pa;
} air_t;

static struct {
    bool started;
    air_t air;
    float density_pad; /* as p / T */
    float x[N];
    float p[N][N];
    uint32_t t_us;
    float noise_var_pa2;
    float last_innovation;
    bool have_last_innovation;
    uint8_t outlier_run;
    float fit, misfit; /* running means of the squared innovation, in sigmas */
    bool reading_run;  /* readings used with no gap between them */
    uint32_t reading_run_since_us, reading_used_us;
    bool blanked;
    uint32_t blank_until_us;
    float loosen_beta;
} self;
_Static_assert(sizeof(self) <= ESTIMATOR_STATE_MAX, "raise ESTIMATOR_STATE_MAX");

/* ── The air above the pad ────────────────────────────────────────── */

static void air_from_pad(air_t *a, float pad_pa, float pad_k) {
    a->pad_pa = pad_pa;
    a->pad_k = pad_k;
    a->pad_m = atmos_altitude_m(pad_pa);
    a->tropopause_k = pad_k - LAPSE * (TROPOPAUSE_M - a->pad_m);
    a->tropopause_pa = pad_pa * powf(a->tropopause_k / pad_k, G0 / (R_AIR * LAPSE));
    a->stratosphere_pa = a->tropopause_pa * expf(-G0 * (STRATOSPHERE_M - TROPOPAUSE_M) / (R_AIR * a->tropopause_k));
}

static float air_temperature_k(const air_t *a, float h) {
    float z = a->pad_m + h;
    if (z <= TROPOPAUSE_M)
        return a->pad_k - LAPSE * h;
    if (z <= STRATOSPHERE_M)
        return a->tropopause_k;
    return a->tropopause_k + STRATO_LAPSE * (z - STRATOSPHERE_M);
}

static float air_pressure_pa(const air_t *a, float h) {
    float z = a->pad_m + h;
    float k = air_temperature_k(a, h);
    if (z <= TROPOPAUSE_M)
        return a->pad_pa * powf(k / a->pad_k, G0 / (R_AIR * LAPSE));
    if (z <= STRATOSPHERE_M)
        return a->tropopause_pa * expf(-G0 * (z - TROPOPAUSE_M) / (R_AIR * a->tropopause_k));
    return a->stratosphere_pa * powf(a->tropopause_k / k, G0 / (R_AIR * STRATO_LAPSE));
}

/* ── The model ────────────────────────────────────────────────────── */

static float softplus(float s) {
    return s > 30.0f ? s : log1pf(expf(s));
}

static float softplus_slope(float s) {
    return s > 30.0f ? 1.0f : 1.0f / (1.0f + expf(-s));
}

static float thrust_of(float s) {
    return A0 * softplus(s);
}

/* The drag coefficient at this height: beta, by the air's density there. */
static float drag_at(float h, float ln_beta, float *scale_height_m) {
    float k = air_temperature_k(&self.air, h);
    *scale_height_m = R_AIR * k / G0;
    return expf(ln_beta) * (air_pressure_pa(&self.air, h) / k) / self.density_pad;
}

static void propagate_covariance(const float f[N][N]) {
    float fp[N][N], out[N][N];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int m = 0; m < N; m++)
                sum += f[i][m] * self.p[m][j];
            fp[i][j] = sum;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int m = 0; m < N; m++)
                sum += fp[i][m] * f[j][m];
            out[i][j] = sum;
        }
    memcpy(self.p, out, sizeof(out));
}

/* The parameters loosen while the model fails to explain the readings. */
static float loosening(void) {
    return fminf(LOOSEN_MAX, fmaxf(1.0f, self.misfit));
}

/* Drag is taken implicitly: a canopy opening at speed is stiff. */
static void step(float dt) {
    float h = self.x[H], v = self.x[V], s = self.x[S], lb = self.x[LB];
    float scale_height_m;
    float k = drag_at(h, lb, &scale_height_m);
    float speed = fabsf(v);
    float den = 1.0f + k * speed * dt;
    float u = v + (thrust_of(s) - G0) * dt;
    float v1 = u / den;
    float shared = u * k * dt / (den * den);

    float g_v = fmaxf(1.0f / den - (v >= 0.0f ? shared : -shared), 0.0f);
    float g_s = A0 * softplus_slope(s) * dt / den;
    float g_lb = -shared * speed;
    float g_h = shared * speed / scale_height_m;
    float fade = dt / THRUST_TAU_S;
    float pull = dt / BETA_TAU_S;
    const float f[N][N] = {
        {1.0f + g_h * dt, g_v * dt, g_s * dt, g_lb * dt},
        {g_h, g_v, g_s, g_lb},
        {0.0f, 0.0f, 1.0f - fade, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f - pull},
    };
    propagate_covariance(f);
    self.p[V][V] += SPEED_NOISE * dt;
    self.p[S][S] += S_NOISE * S_NOISE * dt * loosening();
    self.p[LB][LB] += BETA_NOISE * BETA_NOISE * dt + 2.0f * pull * BETA_PRIOR_SD * BETA_PRIOR_SD + self.loosen_beta;
    self.loosen_beta = 0.0f;

    self.x[H] = h + v1 * dt;
    self.x[V] = v1;
    self.x[S] = s + (S_MIN - s) * fade;
    self.x[LB] = lb + (logf(BETA_PRIOR) - lb) * pull;
}

/* The model has no ground, and readings supply it. With none, a prediction
 * that reaches the pad's level stops there: it does not fall on through. */
static void stop_at_the_ground(void) {
    if (self.reading_run || self.x[H] >= 0.0f)
        return;
    self.x[H] = 0.0f;
    self.x[V] = fmaxf(self.x[V], 0.0f);
}

/* Returns the seconds stepped over. */
static float predict_to(uint32_t t_us) {
    int32_t dt_us = (int32_t)(t_us - self.t_us);
    if (dt_us <= 0)
        return 0.0f;
    self.t_us = t_us;
    float dt = (float)dt_us * 1e-6f;
    for (float left = dt; left > 0.0f; left -= STEP_MAX_S) {
        step(fminf(left, STEP_MAX_S));
        stop_at_the_ground();
    }
    return dt;
}

static void keep_parameters_in_range(void) {
    self.x[S] = fminf(fmaxf(self.x[S], S_MIN), S_MAX);
    self.x[LB] = fminf(fmaxf(self.x[LB], LN_BETA_MIN), LN_BETA_MAX);
}

/* ── The reading ──────────────────────────────────────────────────── */

static void correct(float innovation, float gain_h, float variance) {
    float gain[N], row[N];
    for (int i = 0; i < N; i++) {
        gain[i] = self.p[i][H] * gain_h / variance;
        row[i] = self.p[H][i];
    }
    for (int i = 0; i < N; i++) {
        self.x[i] += gain[i] * innovation;
        for (int j = 0; j < N; j++)
            self.p[i][j] -= gain[i] * gain_h * row[j];
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < i; j++)
            self.p[i][j] = self.p[j][i] = 0.5f * (self.p[i][j] + self.p[j][i]);
}

static void track_noise(float innovation, float reading_pa, float dt) {
    if (self.have_last_innovation) {
        float d = (innovation - self.last_innovation) * reading_pa;
        self.noise_var_pa2 += fminf(1.0f, dt / NOISE_TAU_S) * (0.5f * d * d - self.noise_var_pa2);
        self.noise_var_pa2 = fmaxf(self.noise_var_pa2, PEST_NOISE_FLOOR_PA * PEST_NOISE_FLOOR_PA);
    }
}

static void take(float reading_pa, float dt) {
    float predicted_pa = air_pressure_pa(&self.air, self.x[H]);
    float gain_h = -G0 / (R_AIR * air_temperature_k(&self.air, self.x[H])); /* d(ln p)/dh */
    float innovation = log1pf((reading_pa - predicted_pa) / predicted_pa);
    float variance = gain_h * gain_h * self.p[H][H] + self.noise_var_pa2 / (reading_pa * reading_pa);
    float sigmas = innovation / sqrtf(variance);
    bool far = fabsf(sigmas) > GATE_SIGMAS;

    /* One or two readings far off are not used, and say nothing of the model. */
    if (far && self.outlier_run < OUTLIER_RUN_MAX) {
        self.outlier_run++;
        self.have_last_innovation = false;
        return;
    }
    float squared = fminf(sigmas * sigmas, SQUARED_MAX);
    self.misfit += fminf(1.0f, dt / ADAPT_TAU_S) * (squared - self.misfit);
    self.fit += fminf(1.0f, dt / FIT_TAU_S) * (squared - self.fit);
    if (!self.reading_run) {
        self.reading_run = true;
        self.reading_run_since_us = self.t_us;
    }
    self.reading_used_us = self.t_us;
    if (!far) {
        self.outlier_run = 0;
        track_noise(innovation, reading_pa, dt);
    }
    self.last_innovation = innovation;
    self.have_last_innovation = true;
    correct(innovation, gain_h, variance);
}

/* ── The interface ────────────────────────────────────────────────── */

static void start(int32_t reading_pa, uint32_t t_us, float pad_temp_k) {
    memset(&self, 0, sizeof(self));
    float pad_pa = (float)reading_pa;
    air_from_pad(&self.air, pad_pa, pad_temp_k > 0.0f ? pad_temp_k : atmos_temperature_k(pad_pa));
    self.density_pad = pad_pa / self.air.pad_k;
    self.x[S] = logf(expm1f(G0 / A0)); /* at rest: the pad holds it up at 1 g */
    self.x[LB] = logf(BETA_PRIOR);
    float h_sigma = (R_AIR * self.air.pad_k / G0) * PEST_NOISE_FLOOR_PA / pad_pa;
    self.p[H][H] = h_sigma * h_sigma;
    self.p[V][V] = 0.01f;
    self.p[S][S] = 0.01f;
    self.p[LB][LB] = BETA_PRIOR_SD * BETA_PRIOR_SD;
    self.noise_var_pa2 = PEST_NOISE_FLOOR_PA * PEST_NOISE_FLOOR_PA;
    self.fit = self.misfit = 1.0f;
    self.t_us = self.reading_used_us = t_us;
    self.started = true;
}

static void end_run_at_a_gap(void) {
    if (self.t_us - self.reading_used_us > GAP_US)
        self.reading_run = false;
}

static void no_reading(uint32_t t_us) {
    predict_to(t_us);
    keep_parameters_in_range();
    end_run_at_a_gap();
}

static void reading(int32_t reading_pa, uint32_t t_us) {
    if (self.blanked && (int32_t)(t_us - self.blank_until_us) < 0) {
        no_reading(t_us);
        return;
    }
    self.blanked = false;
    float dt = predict_to(t_us);
    if (dt <= 0.0f)
        return;
    end_run_at_a_gap();
    take((float)reading_pa, dt);
    keep_parameters_in_range();
}

static void pulse(uint32_t t_us) {
    self.blanked = true;
    self.blank_until_us = t_us + PULSE_BLANK_US;
    self.loosen_beta = PULSE_LOOSENS;
}

static void estimate(estimate_t *out) {
    float h = self.x[H], v = self.x[V];
    float scale_height_m;
    float k = drag_at(h, self.x[LB], &scale_height_m);
    float accel = thrust_of(self.x[S]) - G0 - k * v * fabsf(v);
    out->pressure_pa = air_pressure_pa(&self.air, h);
    out->rate = -v / scale_height_m;
    out->curve = -accel / scale_height_m;
    out->rate_sigma = sqrtf(fmaxf(self.p[V][V], 0.0f)) / scale_height_m;
    out->log_sigma = sqrtf(fmaxf(self.p[H][H], 0.0f)) / scale_height_m;
    out->noise_pa = sqrtf(self.noise_var_pa2);
    out->explains = self.fit < FIT_LIMIT && self.reading_run && self.t_us - self.reading_run_since_us >= EVIDENCE_US;
}

const estimator_vt estimator_lumped_vt = {
    .name = "lumped",
    .start = start,
    .reading = reading,
    .no_reading = no_reading,
    .pulse = pulse,
    .estimate = estimate,
    .state = &self,
    .state_size = sizeof(self),
};
