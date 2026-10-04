/*
 * "constacc": the constant-acceleration Kalman filter of DD-085
 * (pressure_estimator.c), behind the estimator interface.
 *
 * Its model explains the readings when none has been far from its prediction
 * for a second and its curvature is one a body near the top of its flight
 * can have (docs/apogee_without_a_mach_flag.md).
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimator.h"
#include "pressure_estimator.h"
#include <math.h>

#define ROUGH_SIGMAS 4.0f
#define SMOOTH_US 1000000u
#define ARC_CURVE_MAX 0.005f /* d2(ln p)/dt2 of about 3.5 g */
#define GAP_US 250000u       /* [SNS-PRES-11] */

static struct {
    pest_t filter;
    bool rough;
    uint32_t rough_until_us;
} self;
_Static_assert(sizeof(self) <= ESTIMATOR_STATE_MAX, "raise ESTIMATOR_STATE_MAX");

static void hold_rough_from(uint32_t t_us) {
    self.rough = true;
    self.rough_until_us = t_us + SMOOTH_US;
}

static void start(int32_t reading_pa, uint32_t t_us, float pad_temp_k) {
    (void)pad_temp_k;
    pest_start(&self.filter, reading_pa, t_us, PEST_NOISE_FLOOR_PA);
    hold_rough_from(t_us);
}

static void reading(int32_t reading_pa, uint32_t t_us) {
    bool after_a_gap = t_us - self.filter.t_us > GAP_US;
    pest_update(&self.filter, reading_pa, t_us);
    bool off_the_arc =
        after_a_gap || fabsf(self.filter.innovation_sigmas) > ROUGH_SIGMAS || fabsf(self.filter.curve) >= ARC_CURVE_MAX;
    if (off_the_arc)
        hold_rough_from(t_us);
    else if (self.rough && (int32_t)(t_us - self.rough_until_us) >= 0)
        self.rough = false;
}

static void no_reading(uint32_t t_us) {
    if (t_us - self.filter.t_us > GAP_US)
        hold_rough_from(t_us);
}

static void pulse(uint32_t t_us) {
    (void)t_us;
}

static void estimate(estimate_t *out) {
    pest_estimate_t e = pest_estimate(&self.filter);
    out->pressure_pa = e.pressure_pa;
    out->rate = e.rate;
    out->curve = e.curve;
    out->rate_sigma = e.rate_sigma;
    out->log_sigma = e.log_sigma;
    out->noise_pa = e.noise_pa;
    out->explains = !self.rough;
}

const estimator_vt estimator_constacc_vt = {
    .name = "constacc",
    .start = start,
    .reading = reading,
    .no_reading = no_reading,
    .pulse = pulse,
    .estimate = estimate,
    .state = &self,
    .state_size = sizeof(self),
};
