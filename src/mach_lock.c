/*
 * See mach_lock.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mach_lock.h"
#include "hold.h"
#include "mach_lockout.h"

#define RELEASE_HOLD_MS 1000u /* [FLT-MACH-03] */
#define FALLING_HOLD_MS 2000u /* [FLT-MACH-04] */
#define PAD_FLAG_FORGET_MS 10000u

void mach_lock_set(mach_lock_t *m, float pressure_pa, uint32_t sample_ms) {
    m->flagged = true;
    m->flag_pressure_pa = pressure_pa;
    m->flag_ms = sample_ms;
    m->coasting_since = 0;
    m->falling_since = 0;
}

/* Before any release the raw readings' own rate sets the flag as well: the
 * filter is still catching a hard boost when the rocket passes Mach 0.85.
 * After a release only the filtered rate does, so one bad reading cannot
 * lock out the apogee just ahead. */
static bool flag_due(const mach_lock_t *m, const pp_sample_t *s) {
    if (s->suspect)
        return false;
    if (m->released_once)
        return mach_too_fast(s->rate);
    return mach_too_fast(s->rate) || mach_too_fast(s->short_rate);
}

void mach_lock_on_pad(mach_lock_t *m, const pp_sample_t *s) {
    if (m->flagged && !s->risen && s->timestamp_ms - m->flag_ms >= PAD_FLAG_FORGET_MS) {
        *m = (mach_lock_t){0};
        return;
    }
    if (!m->flagged && s->risen && flag_due(m, s))
        mach_lock_set(m, s->pressure_pa, s->timestamp_ms);
}

static void release(mach_lock_t *m, uint32_t sample_ms) {
    m->flagged = false;
    m->released_once = true;
    m->release_ms = sample_ms;
}

mach_event_t mach_lock_in_ascent(mach_lock_t *m, const pp_sample_t *s) {
    uint32_t ts = s->timestamp_ms;
    if (!m->flagged) {
        if (!flag_due(m, s))
            return MACH_NO_EVENT;
        mach_lock_set(m, s->pressure_pa, ts);
        return MACH_FLAGGED;
    }
    bool believable = s->smooth && !s->suspect;
    bool coasting = believable && mach_slow_ascent(s->rate) && mach_under_gravity(s->rate, s->curve);
    if (held_for(coasting, &m->coasting_since, ts, RELEASE_HOLD_MS)) {
        release(m, ts);
        return MACH_RELEASED;
    }
    /* [FLT-MACH-04] Past apogee and falling as gravity gives, or back below
     * where the flag was set: either is a descent no port error explains. */
    bool falling = believable && mach_slow_descent(s->rate) && mach_under_gravity(s->rate, s->curve);
    bool below_flag = believable && s->rate > 0.0f && s->pressure_pa > m->flag_pressure_pa;
    if (held_for(falling, &m->falling_since, ts, FALLING_HOLD_MS) || below_flag) {
        m->flagged = false;
        m->fell_back = true;
        return MACH_APOGEE;
    }
    return MACH_NO_EVENT;
}
