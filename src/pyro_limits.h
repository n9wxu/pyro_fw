/*
 * What a board permits of the pyro timing fields [PYR-BOARD-01].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_LIMITS_H
#define PYRO_LIMITS_H

#include <stdint.h>

typedef struct {
    uint16_t refire_interval_default_ms, refire_interval_min_ms, refire_interval_max_ms;
    uint16_t fire_gap_default_ms, fire_gap_min_ms, fire_gap_max_ms;
} pyro_limits_t;

/* The general values, where a board declares none. */
#define PYRO_LIMITS_GENERAL                                                                                            \
    { 1000, 500, 10000, 3000, 1000, 10000 }

#endif
