/*
 * The pad's pressure [GND-CAL-01..07].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GROUND_REFERENCE_H
#define GROUND_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#define GREF_WINDOW_MS 5000u
#define GREF_BLOCK_MS 250u
#define GREF_BLOCKS (GREF_WINDOW_MS / GREF_BLOCK_MS)
#define GREF_MAX_DEVIATION_PA 50 /* [GND-CAL-03] */

typedef struct {
    bool tracking;
    int32_t pressure_pa;
    int64_t block_sum[GREF_BLOCKS];
    uint16_t block_count[GREF_BLOCKS];
    uint32_t block_start_ms[GREF_BLOCKS];
    uint8_t current;
    bool current_started;
    uint8_t blocks_filled;
    bool degraded;
    uint32_t rejecting_since_ms; /* one past the time, so 0 can mean "not rejecting" */
    uint32_t reseeds;
} gref_t;

void gref_seed(gref_t *g, int32_t pressure_pa);
void gref_hold(gref_t *g, int32_t pressure_pa); /* a known ground, not tracked: a resumed flight */
void gref_feed(gref_t *g, int32_t pressure_pa, uint32_t t_ms);
int32_t gref_pressure(const gref_t *g);

/* [GND-CAL-04] False when the reference rests on less than a second of the pad. */
bool gref_freeze_before(gref_t *g, uint32_t t_ms);
bool gref_degraded(const gref_t *g);
uint32_t gref_window_ms(const gref_t *g);

/* [GND-CAL-06] */
uint32_t gref_rejecting_ms(const gref_t *g, uint32_t now_ms);
void gref_reseed(gref_t *g, int32_t pressure_pa);

#endif
