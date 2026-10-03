/*
 * Launch [FLT-LAUNCH-02, FLT-LAUNCH-07].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LAUNCH_DETECTOR_H
#define LAUNCH_DETECTOR_H

#include "pressure_processing.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t held_since;
} launch_detector_t;

bool launch_detected(launch_detector_t *d, const pp_sample_t *s, float pad_pa);

#endif
