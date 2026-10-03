/*
 * The Mach lock: when the pressure may not be believed about apogee
 * [FLT-MACH-02..07, DD-049, DD-085]. See docs/mach_lockout.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MACH_LOCK_H
#define MACH_LOCK_H

#include "pressure_processing.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool flagged;
    bool released_once;
    bool fell_back;
    float flag_pressure_pa;
    uint32_t flag_ms; /* 0: never flagged */
    uint32_t release_ms;
    uint32_t coasting_since, falling_since;
} mach_lock_t;

typedef enum { MACH_NO_EVENT, MACH_FLAGGED, MACH_RELEASED, MACH_APOGEE } mach_event_t;

/* On the pad, from the first reading of a rise: a hard boost is past Mach
 * 0.85 before the launch is sure. */
void mach_lock_on_pad(mach_lock_t *m, const pp_sample_t *s);
mach_event_t mach_lock_in_ascent(mach_lock_t *m, const pp_sample_t *s);
void mach_lock_set(mach_lock_t *m, float pressure_pa, uint32_t sample_ms);

#endif
