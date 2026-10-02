/*
 * The Mach lockout's thresholds, as pressure ratios in integers [DD-049].
 * Each is derived in docs/mach_lockout.md "Thresholds".
 *
 * A spoiled fit's rate can be anything, so it is clamped: 1000 x 1 MPa/s
 * still fits an int32, and no real flight comes near either clamp.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MACH_LOCKOUT_H
#define MACH_LOCKOUT_H

#include <stdbool.h>
#include <stdint.h>

#define MACH_RATE_CLAMP 1000000 /* Pa/s */
#define MACH_ACCEL_CLAMP 200000 /* Pa/s^2 */

static inline int32_t mach_round_clamp(float x, int32_t lim) {
    if (x >= (float)lim)
        return lim;
    if (x <= -(float)lim)
        return -lim;
    return (int32_t)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

/* Flag: -pdot > 0.029 p, true Mach 0.62-0.76 across the envelope. Physical. */
static inline bool mach_too_fast(int32_t p, float pdot) {
    return 1000 * -mach_round_clamp(pdot, MACH_RATE_CLAMP) > 29 * p;
}

/* Release, slow: climbing, and -pdot < 0.022 p, true Mach 0.47-0.57.
 * Physical. */
static inline bool mach_slow_ascent(int32_t p, float pdot) {
    int32_t r = mach_round_clamp(pdot, MACH_RATE_CLAMP);
    return r < 0 && 1000 * -r < 22 * p;
}

/* Release, decelerating: pddot >= 0.0009 p, 0.58-0.85 g, which gravity alone
 * gives any coasting rocket and no thrusting one shows. Physical. */
static inline bool mach_decelerating(int32_t p, float pddot) {
    return 10000 * mach_round_clamp(pddot, MACH_ACCEL_CLAMP) >= 9 * p;
}

/* Minimum-altitude arm: p < 0.9965 p0, 29-33 m above a 10-45 degree pad.
 * A design constant. */
static inline bool mach_above_arm_height(int32_t p, int32_t p0) {
    return 10000 * p < 9965 * p0;
}

#endif
