/*
 * See altitude_filter.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "altitude_filter.h"
#include <math.h>

/* White-jerk process noise, m^2/s^5. */
#define JERK_PSD 7.0f

/* A reading this far from the prediction is a bad one (bad_run_rejects()). */
#define OUTLIER_SIGMAS 6.0f

#define NOISE_TRACK_S 2.0f
#define INITIAL_SPEED_VAR 64.0f
#define INITIAL_ACCEL_VAR 64.0f

static float squared(float x) {
    return x * x;
}

/* The tracked sensor noise, as altitude at the newest reading, or the
 * reading floor where that is larger. */
static float reading_var(const af_t *f) {
    float sensor = f->noise_var_pa2 * squared(f->m_per_pa);
    float floor = squared(AF_READING_NOISE_M);
    return sensor > floor ? sensor : floor;
}

void af_start(af_t *f, float altitude_m, float m_per_pa, uint32_t t_us) {
    f->started = true;
    f->m_per_pa = m_per_pa;
    f->ref_m = altitude_m;
    f->t_us = t_us;
    f->altitude = 0.0f;
    f->speed = 0.0f;
    f->accel = 0.0f;
    f->noise_var_pa2 = squared(AF_NOISE_FLOOR_PA);
    f->var_hh = reading_var(f);
    f->var_hv = 0.0f;
    f->var_ha = 0.0f;
    f->var_vv = INITIAL_SPEED_VAR;
    f->var_va = 0.0f;
    f->var_aa = INITIAL_ACCEL_VAR;
    f->have_last_innovation = false;
    f->bad_run = (bad_run_t){0};
    f->rejected = false;
    f->innovation_sigmas = 0.0f;
}

static void predict(af_t *f, float dt) {
    float half_dt2 = 0.5f * dt * dt;
    f->altitude += f->speed * dt + f->accel * half_dt2;
    f->speed += f->accel * dt;

    float hh = f->var_hh, hv = f->var_hv, ha = f->var_ha, vv = f->var_vv, va = f->var_va, aa = f->var_aa;
    float dt2 = dt * dt, dt3 = dt2 * dt;
    f->var_hh = hh + 2.0f * dt * hv + 2.0f * half_dt2 * ha + dt2 * vv + 2.0f * dt * half_dt2 * va +
                squared(half_dt2) * aa + JERK_PSD * dt3 * dt2 / 20.0f;
    f->var_hv = hv + dt * (ha + vv) + (dt2 + half_dt2) * va + dt * half_dt2 * aa + JERK_PSD * dt2 * dt2 / 8.0f;
    f->var_ha = ha + dt * va + half_dt2 * aa + JERK_PSD * dt3 / 6.0f;
    f->var_vv = vv + 2.0f * dt * va + dt2 * aa + JERK_PSD * dt3 / 3.0f;
    f->var_va = va + dt * aa + JERK_PSD * dt2 / 2.0f;
    f->var_aa = aa + JERK_PSD * dt;
}

/* The noise is the scatter between successive innovations, which a model
 * running behind the rocket does not change. */
static void track_noise(af_t *f, float innovation, float dt) {
    if (f->have_last_innovation) {
        float step_pa = (innovation - f->last_innovation) / f->m_per_pa;
        float weight = dt / NOISE_TRACK_S;
        if (weight > 1.0f)
            weight = 1.0f;
        f->noise_var_pa2 += weight * (0.5f * squared(step_pa) - f->noise_var_pa2);
        if (f->noise_var_pa2 < squared(AF_NOISE_FLOOR_PA))
            f->noise_var_pa2 = squared(AF_NOISE_FLOOR_PA);
    }
    f->last_innovation = innovation;
    f->have_last_innovation = true;
}

static void correct(af_t *f, float innovation, float innovation_var) {
    float gain_h = f->var_hh / innovation_var;
    float gain_v = f->var_hv / innovation_var;
    float gain_a = f->var_ha / innovation_var;
    f->altitude += gain_h * innovation;
    f->speed += gain_v * innovation;
    f->accel += gain_a * innovation;

    float hh = f->var_hh, hv = f->var_hv, ha = f->var_ha;
    f->var_hh -= gain_h * hh;
    f->var_hv -= gain_h * hv;
    f->var_ha -= gain_h * ha;
    f->var_vv -= gain_v * hv;
    f->var_va -= gain_v * ha;
    f->var_aa -= gain_a * ha;
}

void af_update(af_t *f, float altitude_m, float m_per_pa, uint32_t t_us) {
    if (!f->started)
        return;
    float dt = (float)(int32_t)(t_us - f->t_us) * 1e-6f;
    if (dt <= 0.0f)
        return;
    f->t_us = t_us;
    f->m_per_pa = m_per_pa;
    predict(f, dt);

    float innovation = altitude_m - f->ref_m - f->altitude;
    float innovation_var = f->var_hh + reading_var(f);
    f->innovation_sigmas = innovation / sqrtf(innovation_var);
    bool outlier = fabsf(f->innovation_sigmas) > OUTLIER_SIGMAS;
    f->rejected = bad_run_rejects(&f->bad_run, outlier, t_us);
    if (f->rejected) {
        f->have_last_innovation = false;
        return;
    }
    if (!outlier) {
        track_noise(f, innovation, dt);
    } else {
        f->last_innovation = innovation;
        f->have_last_innovation = true;
    }
    correct(f, innovation, innovation_var);
}

af_estimate_t af_estimate(const af_t *f) {
    af_estimate_t e;
    e.altitude_m = f->ref_m + f->altitude;
    e.speed = f->speed;
    e.accel = f->accel;
    e.speed_sigma = sqrtf(f->var_vv > 0.0f ? f->var_vv : 0.0f);
    e.altitude_sigma = sqrtf(f->var_hh > 0.0f ? f->var_hh : 0.0f);
    e.noise_m = sqrtf(f->noise_var_pa2) * f->m_per_pa;
    e.innovation_sigmas = f->innovation_sigmas;
    return e;
}
