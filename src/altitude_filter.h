/*
 * The constant-acceleration Kalman filter on the readings' altitudes
 * [SNS-EST-01, SNS-EST-02, DD-085, DD-114]: `constacc`, one of the
 * estimators behind estimator.h [DD-092].
 *
 * Its design and what it was measured against are in
 * docs/descent_speed_estimator.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALTITUDE_FILTER_H
#define ALTITUDE_FILTER_H

#include "bad_run.h"
#include <stdbool.h>
#include <stdint.h>

/* The quietest sensor fitted: the tracked noise is never taken below it. */
#define AF_NOISE_FLOOR_PA 1.2f

/* A reading is never weighed as surer than this: what the model leaves out
 * -- gusts, the airframe, the ports -- is metres, not the sensor's
 * decimetres [DD-113]. */
#define AF_READING_NOISE_M 0.5f

typedef struct {
    bool started;
    float ref_m; /* the state is altitude less ref_m, which keeps a float exact enough */
    uint32_t t_us;
    float altitude, speed, accel;
    float var_hh, var_hv, var_ha, var_vv, var_va, var_aa;
    float noise_var_pa2; /* the sensor's, which does not change with height */
    float m_per_pa;      /* the newest reading's */
    float last_innovation;
    bool have_last_innovation;
    bad_run_t bad_run;
    bool rejected; /* the newest reading was in a bad run, and not used */
    float innovation_sigmas;
} af_t;

typedef struct {
    float altitude_m;
    float speed;
    float accel;
    float speed_sigma;
    float altitude_sigma;
    float noise_m;
    float innovation_sigmas; /* how far the newest reading fell from the prediction */
} af_estimate_t;

void af_start(af_t *f, float altitude_m, float m_per_pa, uint32_t t_us);
void af_update(af_t *f, float altitude_m, float m_per_pa, uint32_t t_us);
af_estimate_t af_estimate(const af_t *f);

#endif
