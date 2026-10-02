/*
 * The one estimator [DD-048]: a least-squares quadratic through the last
 * second of pressure samples, evaluated at the newest -- the endpoint, which
 * has no lag on a constant acceleration -- and fitted against each sample's
 * own time [SNS-PRES-08], because a flash stall leaves a gap.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_FIT_H
#define PRESSURE_FIT_H

#include <stdbool.h>
#include <stdint.h>

#define PFIT_WINDOW_US 1000000u
#define PFIT_MIN_SAMPLES 8

typedef struct {
    float p;     /* fitted pressure at the newest sample, Pa */
    float pdot;  /* Pa/s; negative while climbing */
    float pddot; /* Pa/s^2 */
    float rms;   /* residual RMS, Pa */
    float worst; /* largest residual, Pa */
    uint8_t n;
    bool valid; /* at least PFIT_MIN_SAMPLES */
} pfit_t;

/* n samples, oldest first: times in microseconds (they may wrap; only
 * differences are used) and pressures in pascals. */
pfit_t pfit_quadratic(const uint32_t *t_us, const int32_t *p_pa, int n);

/* Clean: residuals no bigger than the sensor's noise explains -- RMS within
 * 2 sigma and every sample within 4 sigma. */
bool pfit_clean(const pfit_t *f, float sigma_pa);

#endif
