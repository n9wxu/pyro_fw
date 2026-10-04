/*
 * Landing [FLT-LAND-02, FLT-LAND-03, FLT-LAND-07].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LANDING_DETECTOR_H
#define LANDING_DETECTOR_H

#include "pressure_processing.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t near_ground_since;
    uint32_t timed_out_since;
} landing_detector_t;

/* descent_ms is how long the descent has lasted; timeout_ms of 0 turns the
 * timeout off. */
bool landing_detected(landing_detector_t *d, const pp_sample_t *s, float pad_pa, uint32_t descent_ms,
                      uint32_t timeout_ms);

#endif
