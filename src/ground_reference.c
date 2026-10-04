/*
 * See ground_reference.h. The window is kept as blocks so the mean of the
 * pad from before a given time can be taken at launch [GND-CAL-04].
 *
 * SPDX-License-Identifier: MIT
 */
#include "ground_reference.h"
#include <string.h>

#define MIN_PAD_MS 1000u /* [GND-CAL-07] */

void gref_seed(gref_t *g, int32_t pressure_pa) {
    uint32_t reseeds = g->reseeds;
    memset(g, 0, sizeof(*g));
    g->reseeds = reseeds;
    g->tracking = true;
    g->pressure_pa = pressure_pa;
    g->block_sum[0] = pressure_pa;
    g->block_count[0] = 1;
    g->blocks_filled = 1;
}

void gref_hold(gref_t *g, int32_t pressure_pa) {
    gref_seed(g, pressure_pa);
    g->tracking = false;
}

static bool within_gate(const gref_t *g, int32_t pressure_pa) {
    int32_t deviation = pressure_pa - g->pressure_pa;
    return deviation <= GREF_MAX_DEVIATION_PA && deviation >= -GREF_MAX_DEVIATION_PA;
}

static void open_next_block(gref_t *g, uint32_t t_ms) {
    g->current = (uint8_t)((g->current + 1u) % GREF_BLOCKS);
    g->block_sum[g->current] = 0;
    g->block_count[g->current] = 0;
    g->block_start_ms[g->current] = t_ms;
    if (g->blocks_filled < GREF_BLOCKS)
        g->blocks_filled++;
}

static int32_t mean_of_blocks(const gref_t *g, int32_t fallback_pa) {
    int64_t total = 0;
    uint32_t count = 0;
    for (unsigned i = 0; i < GREF_BLOCKS; i++) {
        total += g->block_sum[i];
        count += g->block_count[i];
    }
    return count ? (int32_t)(total / (int64_t)count) : fallback_pa;
}

void gref_feed(gref_t *g, int32_t pressure_pa, uint32_t t_ms) {
    if (!g->tracking)
        return;
    if (!within_gate(g, pressure_pa)) {
        if (g->rejecting_since_ms == 0)
            g->rejecting_since_ms = t_ms + 1u;
        return;
    }
    g->rejecting_since_ms = 0;
    if (!g->current_started) {
        g->current_started = true;
        g->block_start_ms[g->current] = t_ms;
    }
    if (t_ms - g->block_start_ms[g->current] >= GREF_BLOCK_MS)
        open_next_block(g, t_ms);
    g->block_sum[g->current] += pressure_pa;
    g->block_count[g->current]++;
    g->pressure_pa = mean_of_blocks(g, g->pressure_pa);
}

int32_t gref_pressure(const gref_t *g) {
    return g->pressure_pa;
}

static bool block_ended_before(const gref_t *g, unsigned i, uint32_t t_ms) {
    return i != g->current && g->block_count[i] != 0 && (int32_t)(t_ms - (g->block_start_ms[i] + GREF_BLOCK_MS)) >= 0;
}

bool gref_freeze_before(gref_t *g, uint32_t t_ms) {
    g->tracking = false;
    int64_t total = 0;
    uint32_t count = 0, span_ms = 0;
    for (unsigned i = 0; i < GREF_BLOCKS; i++) {
        if (!block_ended_before(g, i, t_ms))
            continue;
        total += g->block_sum[i];
        count += g->block_count[i];
        span_ms += GREF_BLOCK_MS;
    }
    if (count > 0)
        g->pressure_pa = (int32_t)(total / (int64_t)count);
    g->degraded = span_ms < MIN_PAD_MS;
    return !g->degraded;
}

bool gref_degraded(const gref_t *g) {
    return g->degraded;
}

uint32_t gref_window_ms(const gref_t *g) {
    return (uint32_t)g->blocks_filled * GREF_BLOCK_MS;
}

uint32_t gref_rejecting_ms(const gref_t *g, uint32_t now_ms) {
    return (g->tracking && g->rejecting_since_ms) ? now_ms + 1u - g->rejecting_since_ms : 0;
}

void gref_reseed(gref_t *g, int32_t pressure_pa) {
    gref_seed(g, pressure_pa);
    g->reseeds++;
}
