/*
 * The Mach lock's thresholds [FLT-MACH-02..06, DD-049].
 *
 * Each is a rate or a curvature of ln(pressure): the same Mach, or the same
 * fraction of g, at any site elevation. docs/mach_lockout.md derives each.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MACH_LOCKOUT_H
#define MACH_LOCKOUT_H

#include <stdbool.h>

#define MACH_FLAG_RATE 0.029f      /* true Mach 0.62 to 0.76 */
#define MACH_RELEASE_RATE 0.022f   /* true Mach 0.47 to 0.57 */
#define MACH_GRAVITY_CURVE 0.0009f /* 0.58 g to 0.85 g */
#define MACH_ARM_RATIO 0.9965f     /* about 30 m above the pad */

static inline bool mach_too_fast(float rate) {
    return -rate > MACH_FLAG_RATE;
}

static inline bool mach_slow_ascent(float rate) {
    return rate < 0.0f && -rate < MACH_RELEASE_RATE;
}

static inline bool mach_slow_descent(float rate) {
    return rate > 0.0f && rate < MACH_RELEASE_RATE;
}

/* Slowing upward, or gathering speed downward, as gravity alone gives. */
static inline bool mach_under_gravity(float rate, float curve) {
    return curve + rate * rate >= MACH_GRAVITY_CURVE;
}

static inline bool mach_above_arm_height(float pressure_pa, float pad_pa) {
    return pressure_pa < MACH_ARM_RATIO * pad_pa;
}

#endif
