/*
 * See pressure_estimator.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_estimator.h"
#include <math.h>

/* White-jerk process noise: 7 m^2/s^5 over the square of an 8 km scale
 * height. One value serves the whole flight: the measurement's weight falls
 * with the pressure, which lengthens the averaging as the air thins. */
#define JERK_PSD 1.09375e-7f

/* A reading this far from the prediction is not used, unless it is the third
 * in a row: one or two are bad readings, a run is data [SNS-EST-02]. */
#define OUTLIER_SIGMAS 6.0f
#define OUTLIER_RUN_MAX 2

#define NOISE_TRACK_S 2.0f
#define INITIAL_RATE_VAR 1e-6f
#define INITIAL_CURVE_VAR 1e-6f

static float squared(float x) {
    return x * x;
}

void pest_start(pest_t *f, int32_t reading_pa, uint32_t t_us, float noise_pa) {
    float p = (float)reading_pa;
    f->started = true;
    f->ref_pa = p;
    f->t_us = t_us;
    f->log_p = 0.0f;
    f->rate = 0.0f;
    f->curve = 0.0f;
    f->noise_var_pa2 = squared(noise_pa > PEST_NOISE_FLOOR_PA ? noise_pa : PEST_NOISE_FLOOR_PA);
    f->var_pp = f->noise_var_pa2 / squared(p);
    f->var_pr = 0.0f;
    f->var_pc = 0.0f;
    f->var_rr = INITIAL_RATE_VAR;
    f->var_rc = 0.0f;
    f->var_cc = INITIAL_CURVE_VAR;
    f->have_last_innovation = false;
    f->outlier_run = 0;
    f->innovation_sigmas = 0.0f;
}

static void predict(pest_t *f, float dt) {
    float half_dt2 = 0.5f * dt * dt;
    f->log_p += f->rate * dt + f->curve * half_dt2;
    f->rate += f->curve * dt;

    float pp = f->var_pp, pr = f->var_pr, pc = f->var_pc, rr = f->var_rr, rc = f->var_rc, cc = f->var_cc;
    float dt2 = dt * dt, dt3 = dt2 * dt;
    f->var_pp = pp + 2.0f * dt * pr + 2.0f * half_dt2 * pc + dt2 * rr + 2.0f * dt * half_dt2 * rc +
                squared(half_dt2) * cc + JERK_PSD * dt3 * dt2 / 20.0f;
    f->var_pr = pr + dt * (pc + rr) + (dt2 + half_dt2) * rc + dt * half_dt2 * cc + JERK_PSD * dt2 * dt2 / 8.0f;
    f->var_pc = pc + dt * rc + half_dt2 * cc + JERK_PSD * dt3 / 6.0f;
    f->var_rr = rr + 2.0f * dt * rc + dt2 * cc + JERK_PSD * dt3 / 3.0f;
    f->var_rc = rc + dt * cc + JERK_PSD * dt2 / 2.0f;
    f->var_cc = cc + JERK_PSD * dt;
}

/* The noise is the scatter between successive innovations, which a model
 * running behind the rocket does not change. */
static void track_noise(pest_t *f, float innovation, float pressure_pa, float dt) {
    if (f->have_last_innovation) {
        float step_pa = (innovation - f->last_innovation) * pressure_pa;
        float weight = dt / NOISE_TRACK_S;
        if (weight > 1.0f)
            weight = 1.0f;
        f->noise_var_pa2 += weight * (0.5f * squared(step_pa) - f->noise_var_pa2);
        if (f->noise_var_pa2 < squared(PEST_NOISE_FLOOR_PA))
            f->noise_var_pa2 = squared(PEST_NOISE_FLOOR_PA);
    }
    f->last_innovation = innovation;
    f->have_last_innovation = true;
}

static void correct(pest_t *f, float innovation, float innovation_var) {
    float gain_p = f->var_pp / innovation_var;
    float gain_r = f->var_pr / innovation_var;
    float gain_c = f->var_pc / innovation_var;
    f->log_p += gain_p * innovation;
    f->rate += gain_r * innovation;
    f->curve += gain_c * innovation;

    float pp = f->var_pp, pr = f->var_pr, pc = f->var_pc;
    f->var_pp -= gain_p * pp;
    f->var_pr -= gain_p * pr;
    f->var_pc -= gain_p * pc;
    f->var_rr -= gain_r * pr;
    f->var_rc -= gain_r * pc;
    f->var_cc -= gain_c * pc;
}

void pest_update(pest_t *f, int32_t reading_pa, uint32_t t_us) {
    if (!f->started || reading_pa <= 0)
        return;
    float dt = (float)(int32_t)(t_us - f->t_us) * 1e-6f;
    if (dt <= 0.0f)
        return;
    f->t_us = t_us;
    predict(f, dt);

    float reading = (float)reading_pa;
    float innovation = logf(reading / f->ref_pa) - f->log_p;
    float innovation_var = f->var_pp + f->noise_var_pa2 / squared(reading);
    f->innovation_sigmas = innovation / sqrtf(innovation_var);
    bool outlier = fabsf(f->innovation_sigmas) > OUTLIER_SIGMAS;

    if (outlier && f->outlier_run < OUTLIER_RUN_MAX) {
        f->outlier_run++;
        f->have_last_innovation = false;
        return;
    }
    if (!outlier) {
        f->outlier_run = 0;
        track_noise(f, innovation, reading, dt);
    } else {
        f->last_innovation = innovation;
        f->have_last_innovation = true;
    }
    correct(f, innovation, innovation_var);
}

pest_estimate_t pest_estimate(const pest_t *f) {
    pest_estimate_t e;
    e.pressure_pa = f->ref_pa * expf(f->log_p);
    e.rate = f->rate;
    e.curve = f->curve;
    e.rate_sigma = sqrtf(f->var_rr > 0.0f ? f->var_rr : 0.0f);
    e.log_sigma = sqrtf(f->var_pp > 0.0f ? f->var_pp : 0.0f);
    e.noise_pa = sqrtf(f->noise_var_pa2);
    e.innovation_sigmas = f->innovation_sigmas;
    return e;
}
