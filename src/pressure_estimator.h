/*
 * The one estimator of the pressure [SNS-EST-01, SNS-EST-02, DD-085].
 *
 * A Kalman filter on the raw readings. Its design and what it was measured
 * against are in docs/descent_speed_estimator.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_ESTIMATOR_H
#define PRESSURE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

/* The quietest sensor fitted: the tracked noise is never taken below it. */
#define PEST_NOISE_FLOOR_PA 1.2f

typedef struct {
    bool started;
    float ref_pa; /* the state is ln(p / ref_pa), which keeps a float exact enough */
    uint32_t t_us;
    float log_p, rate, curve;
    float var_pp, var_pr, var_pc, var_rr, var_rc, var_cc;
    float noise_var_pa2;
    float last_innovation;
    bool have_last_innovation;
    uint8_t outlier_run;
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
