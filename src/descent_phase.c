/*
 * See descent_phase.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "descent_phase.h"

#define DROGUE_MAX_CMS 3500
#define MAIN_MAX_CMS 1000
/* Free fall gains about 12 m/s over the dwell, which no canopy's rate
 * tolerates: that is what tells a canopy from a fall passing through its
 * band. */
#define DWELL_MS 1200u
#define TOLERANCE_MIN_CMS 250
#define TOLERANCE_FRACTION 4
#define LOST_HOLD_MS 1000u

static descent_band_t band_of(int32_t rate_cms) {
    if (rate_cms <= MAIN_MAX_CMS)
        return DESCENT_UNDER_MAIN;
    return rate_cms <= DROGUE_MAX_CMS ? DESCENT_UNDER_DROGUE : DESCENT_FAST;
}

static int32_t tolerance_of(int32_t rate_cms) {
    int32_t tolerance = rate_cms / TOLERANCE_FRACTION;
    return tolerance < TOLERANCE_MIN_CMS ? TOLERANCE_MIN_CMS : tolerance;
}

bool descent_phase_settled(descent_phase_t *d, int32_t speed_cms, uint32_t sample_ms, descent_band_t *band) {
    if (speed_cms >= 0) {
        d->band_since = 0;
        return false;
    }
    int32_t rate = -speed_cms;
    descent_band_t now_in = band_of(rate);
    int32_t drift = speed_cms - d->reference_cms;
    if (drift < 0)
        drift = -drift;
    if (now_in != (descent_band_t)d->band || drift > tolerance_of(rate) || d->band_since == 0) {
        d->band = (uint8_t)now_in;
        d->reference_cms = speed_cms;
        d->band_since = sample_ms + 1u;
        return false;
    }
    *band = now_in;
    return sample_ms + 1u - d->band_since >= DWELL_MS;
}

bool descent_phase_lost_drogue(descent_phase_t *d, int32_t speed_cms, uint32_t sample_ms) {
    int32_t rate = speed_cms < 0 ? -speed_cms : speed_cms;
    if (rate <= DROGUE_MAX_CMS) {
        d->exceeded_since = 0;
        return false;
    }
    if (d->exceeded_since == 0) {
        d->exceeded_since = sample_ms + 1u;
        return false;
    }
    return sample_ms + 1u - d->exceeded_since >= LOST_HOLD_MS;
}
