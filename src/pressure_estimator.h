/*
 * The constant-acceleration Kalman filter on the raw pressure readings
 * [SNS-EST-01, SNS-EST-02, DD-085]: `constacc`, one of the estimators behind
 * estimator.h [DD-092].
 *
 * Its design and what it was measured against are in
 * docs/descent_speed_estimator.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_ESTIMATOR_H
#define PRESSURE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

/* The quietest sensor fitted: the tracked noise is never taken below it. */
#define PEST_NOISE_FLOOR_PA 1.2f

/* A reading is never weighed as surer than this, in height: what the model
 * leaves out -- gusts, the airframe, the ports -- is metres, not the
 * sensor's decimetres [DD-113]. */
#define PEST_HEIGHT_NOISE_M 0.5f

/* [SNS-EST-02] Readings far from the prediction are not used for the first
 * 50 ms of their run: the shock of a separation, ours or another board's,
 * lasts that long [DD-113]. A run that goes on is data. */
#define PEST_BAD_RUN_US 50000u

/* One past the time the run began, as hold.h keeps it; 0: no run. */
typedef struct {
    uint32_t since_us;
} pest_bad_run_t;

/* True when a reading this far off, at t_us, is not to be used. */
static inline bool pest_bad_run_rejects(pest_bad_run_t *r, bool far, uint32_t t_us) {
    if (!far) {
        r->since_us = 0;
        return false;
    }
    if (r->since_us == 0)
        r->since_us = t_us + 1u;
    return t_us + 1u - r->since_us < PEST_BAD_RUN_US;
}

typedef struct {
    bool started;
    float ref_pa; /* the state is ln(p / ref_pa), which keeps a float exact enough */
    uint32_t t_us;
    float log_p, rate, curve;
    float var_pp, var_pr, var_pc, var_rr, var_rc, var_cc;
    float noise_var_pa2;
    float last_innovation;
    bool have_last_innovation;
    pest_bad_run_t bad_run;
    bool rejected; /* the newest reading was in a bad run, and not used */
    float innovation_sigmas;
} pest_t;

typedef struct {
    float pressure_pa;
    float rate;              /* d(ln p)/dt, 1/s: negative while climbing */
    float curve;             /* d2(ln p)/dt2, 1/s^2 */
    float rate_sigma;        /* one standard deviation of rate */
    float log_sigma;         /* one standard deviation of ln(pressure) */
    float noise_pa;          /* the sensor noise being tracked */
    float innovation_sigmas; /* how far the newest reading fell from the prediction */
} pest_estimate_t;

void pest_start(pest_t *f, int32_t reading_pa, uint32_t t_us, float noise_pa);
void pest_update(pest_t *f, int32_t reading_pa, uint32_t t_us);
pest_estimate_t pest_estimate(const pest_t *f);

#endif
