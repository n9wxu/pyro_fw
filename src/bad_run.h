/*
 * The gate on a run of readings far from a filter's prediction [SNS-EST-02].
 *
 * Such readings are not used for the first 50 ms of their run: the shock of
 * a separation, ours or another board's, lasts that long [DD-113]. A run
 * that goes on is data.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BAD_RUN_H
#define BAD_RUN_H

#include <stdbool.h>
#include <stdint.h>

#define BAD_RUN_US 50000u

/* One past the time the run began, as hold.h keeps it; 0: no run. */
typedef struct {
    uint32_t since_us;
} bad_run_t;

/* True when a reading this far off, at t_us, is not to be used. */
static inline bool bad_run_rejects(bad_run_t *r, bool far, uint32_t t_us) {
    if (!far) {
        r->since_us = 0;
        return false;
    }
    if (r->since_us == 0)
        r->since_us = t_us + 1u;
    return t_us + 1u - r->since_us < BAD_RUN_US;
}

#endif
