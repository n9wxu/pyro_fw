/*
 * See launch_detector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "launch_detector.h"

#define LAUNCH_HEIGHT_M 15.0f

bool launch_detected(const pp_sample_t *s) {
    return s->height_m > LAUNCH_HEIGHT_M;
}
