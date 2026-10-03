/*
 * Apogee from an estimator's state [FLT-APO-01, FLT-APO-07, DD-092].
 *
 * The rocket has gone over the top: seen climbing, then seen falling, with
 * the estimator's model explaining the readings all the way between. A fall
 * whose climb was not seen that way must last.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APOGEE_DETECTOR_H
#define APOGEE_DETECTOR_H

#include "estimator.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool climb_seen;
    bool falling;
    uint32_t falling_since_ms;
} apogee_detector_t;

/* Fed every sample of the flight. believed is false for a reading the sensor's
 * own faults make suspect: nothing is decided on it. */
bool apogee_detected(apogee_detector_t *d, const estimate_t *e, bool believed, uint32_t sample_ms);

#endif
