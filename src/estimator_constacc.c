/*
 * "constacc": the constant-acceleration Kalman filter of DD-085
 * (altitude_filter.c), behind the estimator interface.
 *
 * Its model explains the readings when none has been far from its prediction
 * for a second and its acceleration is one a body near the top of its flight
 * can have (docs/apogee_without_a_mach_flag.md).
 *
 * SPDX-License-Identifier: MIT
 */
#include "estimator.h"
#include "altitude_filter.h"
#include <math.h>

#define ROUGH_SIGMAS 4.0f
#define SMOOTH_US 1000000u
#define ARC_ACCEL_MAX 34.3f /* 3.5 g */
#define GAP_US 250000u      /* [SNS-PRES-11] */

static struct {
    af_t filter;
    bool rough;
    uint32_t rough_until_us;
} self;
_Static_assert(sizeof(self) <= ESTIMATOR_STATE_MAX, "raise ESTIMATOR_STATE_MAX");

static void hold_rough_from(uint32_t t_us) {
    self.rough = true;
    self.rough_until_us = t_us + SMOOTH_US;
}

static void start(float altitude_m, float m_per_pa, uint32_t t_us, float pad_temp_k) {
    (void)pad_temp_k;
    af_start(&self.filter, altitude_m, m_per_pa, t_us);
    hold_rough_from(t_us);
}

static void reading(float altitude_m, float m_per_pa, uint32_t t_us) {
    bool after_a_gap = t_us - self.filter.t_us > GAP_US;
    af_update(&self.filter, altitude_m, m_per_pa, t_us);
    bool off_the_arc = after_a_gap || fabsf(self.filter.accel) >= ARC_ACCEL_MAX ||
                       (!self.filter.rejected && fabsf(self.filter.innovation_sigmas) > ROUGH_SIGMAS);
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
    af_estimate_t e = af_estimate(&self.filter);
    out->altitude_m = e.altitude_m;
    out->speed = e.speed;
    out->accel = e.accel;
    out->speed_sigma = e.speed_sigma;
    out->altitude_sigma = e.altitude_sigma;
    out->noise_m = e.noise_m;
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
