/*
 * The launch trigger [FLT-LAUNCH-02, FLT-LAUNCH-07].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LAUNCH_DETECTOR_H
#define LAUNCH_DETECTOR_H

#include "pressure_processing.h"
#include <stdbool.h>

/* A pure function of the filtered state. */
bool launch_detected(const pp_sample_t *s);

#endif
