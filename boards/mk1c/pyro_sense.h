/*
 * MK1C tracking-test classification [DESIGN.md S3, DD-054]. Pure arithmetic
 * on ADC counts, shared by pyro_board.c and the host tests.
 *
 * Presence is a channel node following the bus, judged against the bus
 * reading taken in the same test, never against an absolute level. The
 * bench MK1C's bus sits at about 688 counts under bias, not DESIGN.md 4's
 * 1058, because U9's OUT conducts back into the part above about 0.72 V,
 * and that knee moves with the part and its temperature. A present channel
 * reads the bus less its match; an open one reads under 50 counts.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_SENSE_H
#define PYRO_SENSE_H

#include <stdint.h>

/* ── The bench characterisation (DD-054) ───────────────────────────
 * Fitted to the bench MK1C's ADC and a scope at CN1, 2026-09-26. The plant
 * model is built from these, and every waveform capture carries them so
 * support/pyro_check.py grades the board against what was measured. */
#define MK1C_BENCH_BUS_BIASED_COUNTS 688   /* bus under its bias, no match     */
#define MK1C_BENCH_CH_BIASED_COUNTS 1262   /* channel under its bias, no match */
#define MK1C_BENCH_C_BUS_NF 1100           /* C115 and the bus's strays        */
/* U9's OUT, off, conducting back into the part: a junction behind a resistance. */
#define MK1C_U9_REV_IS_A 1.2e-12
#define MK1C_U9_REV_NVT_V 0.040
#define MK1C_U9_REV_OHM 250
/* Each bias source: a 3.3 V GPIO through a BAT54WS. */
#define MK1C_BIAS_GPIO_V 3.3
#define MK1C_BIAS_DIODE_IS_A 3.7e-6
#define MK1C_BIAS_DIODE_NVT_V 0.045

/* A bus below this never rose: shorted, or its bias open. The test then
 * says nothing about the channels. Every healthy bias puts it above 600. */
#define TRACK_BUS_MIN_COUNTS 200u

typedef enum { TRACK_OPEN, TRACK_PRESENT, TRACK_INVALID } track_t;

/* Present at half the bus or more: a match reads about the whole of it, a
 * 5k dirty connector three quarters, and the raw counts say which. */
static inline track_t track_channel(uint16_t channel, uint16_t bus) {
    if (bus < TRACK_BUS_MIN_COUNTS)
        return TRACK_INVALID;
    return (2u * (uint32_t)channel >= bus) ? TRACK_PRESENT : TRACK_OPEN;
}

#endif /* PYRO_SENSE_H */
