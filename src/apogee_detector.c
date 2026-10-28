/*
 * See apogee_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "apogee_detector.h"
#include "hold.h"
#include <math.h>

#define ERROR_TAU_S 0.16f /* AltOS's 1/16 a reading, at its 100 a second */
#define ERROR_CLAMP_M 127.0f
#define ERROR_BOUND_M 10.0f
#define ERROR_NOISE_SIGMAS 3.0f
#define MOVING_SIGMAS 2.0f /* moving by more than the speed's own uncertainty */
#define UNSEEN_CLIMB_FALL_MS 2000u

/* The square of AltOS's bound, or where the air is thin, of the sensor's own
 * noise. */
static float error_bound_m2(const estimate_t *e) {
    float bound = fmaxf(ERROR_BOUND_M, ERROR_NOISE_SIGMAS * e->noise_m);
    return bound * bound;
}

static void follow_error(apogee_detector_t *d, float error_m, float bound_m2, uint32_t sample_ms) {
    if (bad_run_rejects(&d->bad_run, error_m * error_m >= bound_m2, sample_ms * 1000u))
        return;
    error_m = fminf(error_m, ERROR_CLAMP_M);
    float weight = 1.0f;
    if (d->error_at != 0)
        weight = fminf(1.0f, (float)(sample_ms + 1u - d->error_at) * 1e-3f / ERROR_TAU_S);
    d->error_m2 += weight * (error_m * error_m - d->error_m2);
    d->error_at = sample_ms + 1u;
}

bool apogee_detected(apogee_detector_t *d, const estimate_t *e, float raw_altitude_m, bool believed,
                     uint32_t sample_ms) {
    if (!e->explains) {
        d->climb_seen = false;
        d->falling_since = 0;
    }
    if (!believed)
        return false;
    float bound_m2 = error_bound_m2(e);
    follow_error(d, fabsf(raw_altitude_m - e->altitude_m), bound_m2, sample_ms);
    if (!e->explains)
        return false;
    float margin = MOVING_SIGMAS * e->speed_sigma;
    if (e->speed > margin)
        d->climb_seen = true;
    bool falling = -e->speed > margin && d->error_m2 < bound_m2;
    bool held = held_for(falling, &d->falling_since, sample_ms, UNSEEN_CLIMB_FALL_MS);
    return falling && (d->climb_seen || held);
}
