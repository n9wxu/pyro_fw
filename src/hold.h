/*
 * A condition that must hold for a length of sample time [FLT-RATE-05].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HOLD_H
#define HOLD_H

#include <stdbool.h>
#include <stdint.h>

/* *since is one past the sample time the condition first held, so that a
 * sample time of 0 can start a hold; 0 means it does not hold. */
static inline bool held_for(bool condition, uint32_t *since, uint32_t sample_ms, uint32_t hold_ms) {
    if (!condition) {
        *since = 0;
        return false;
    }
    if (*since == 0)
        *since = sample_ms + 1u;
    return sample_ms + 1u - *since >= hold_ms;
}

#endif
