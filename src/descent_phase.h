/*
 * The descent phase, read from the descent speed holding steady
 * [FLT-DESC-01, FLT-AIR-01, DD-023].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DESCENT_PHASE_H
#define DESCENT_PHASE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { DESCENT_FAST = 0, DESCENT_UNDER_DROGUE, DESCENT_UNDER_MAIN } descent_band_t;

typedef struct {
    uint8_t band;
    uint32_t band_since; /* one past the sample time */
    int32_t reference_cms;
    uint32_t exceeded_since;
} descent_phase_t;

/* speed_cms is in the pad's air, positive up. True once the speed has held
 * in one band, near one value, for the dwell. */
bool descent_phase_settled(descent_phase_t *d, int32_t speed_cms, uint32_t sample_ms, descent_band_t *band);

/* A canopy that was working has stopped: faster than its band, held. */
bool descent_phase_lost_drogue(descent_phase_t *d, int32_t speed_cms, uint32_t sample_ms);

#endif
