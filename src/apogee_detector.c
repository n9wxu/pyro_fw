/*
 * See apogee_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "apogee_detector.h"

#define SIGMAS 3.0f /* moving by more than the rate's own uncertainty */
#define UNSEEN_CLIMB_FALL_MS 2000u

bool apogee_detected(apogee_detector_t *d, const estimate_t *e, bool believed, uint32_t sample_ms) {
    if (!e->explains) {
        *d = (apogee_detector_t){0};
        return false;
    }
    if (!believed)
        return false;
    float margin = SIGMAS * e->rate_sigma;
    if (-e->rate > margin)
        d->climb_seen = true;
    bool falling = e->rate > margin;
    if (falling && !d->falling)
        d->falling_since_ms = sample_ms;
    d->falling = falling;
    return falling && (d->climb_seen || sample_ms - d->falling_since_ms >= UNSEEN_CLIMB_FALL_MS);
}
