/*
 * Apogee from an estimator's state [FLT-APO-01, FLT-APO-07, DD-113].
 *
 * The rocket has gone over the top: the filtered rate is a fall, while the
 * readings stay close to the filtered height. A port error near Mach 1 throws
 * the readings off the filter, and the average of the squared error, which
 * decays reading by reading, holds the decision until they settle.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APOGEE_DETECTOR_H
#define APOGEE_DETECTOR_H

#include "estimator.h"
#include "pressure_estimator.h"
#include <stdbool.h>
#include <stdint.h>

/* Times are one past the sample time, as hold.h keeps them; 0: none. */
typedef struct {
    float error_m2; /* the decaying average of the squared height error */
    uint32_t error_at;
    uint32_t falling_since;
    pest_bad_run_t bad_run; /* a shock's readings are no error of the filter's */
    bool climb_seen;        /* explained, without a break since */
} apogee_detector_t;

/* Fed every sample of the flight, with the raw reading the estimate was
 * formed from. believed is false for a reading the sensor's own faults make
 * suspect: nothing is decided on it. */
bool apogee_detected(apogee_detector_t *d, const estimate_t *e, int32_t raw_pa, bool believed, uint32_t sample_ms);

#endif
