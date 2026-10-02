/*
 * MK1C's sense levels and the presence verdict: pure arithmetic on ADC
 * counts, shared with the plant model and the host tests.
 * See THEORY_OF_OPERATION.md "Sense network".
 * See THEORY_OF_OPERATION.md "Presence test".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_SENSE_H
#define PYRO_SENSE_H

#include <stdint.h>

/* ── Scaling ─────────────────────────────────────────────────────── */

/* Every divider is 0.3329 against a 3.3 V, 12-bit ADC. */
#define NODE_UV_PER_COUNT 2421

static inline uint32_t counts_to_node_mv(uint16_t counts) {
    return ((uint32_t)counts * NODE_UV_PER_COUNT) / 1000u;
}

/* The levels in THEORY_OF_OPERATION.md "Sense network": a new divider or reference breaks them. */
_Static_assert((1058u * NODE_UV_PER_COUNT) / 1000u >= 2550 && (1058u * NODE_UV_PER_COUNT) / 1000u <= 2570,
               "bus bias, no match: 1058 counts should be ~2.56 V");
_Static_assert((1214u * NODE_UV_PER_COUNT) / 1000u >= 2930 && (1214u * NODE_UV_PER_COUNT) / 1000u <= 2950,
               "channel bias, match off: 1214 counts should be ~2.94 V");
_Static_assert((3469u * NODE_UV_PER_COUNT) / 1000u >= 8390 && (3469u * NODE_UV_PER_COUNT) / 1000u <= 8410,
               "full 2S bus: 3469 counts should be ~8.4 V");

/* ── The board as measured (DD-054) ──────────────────────────────── */

#define MK1C_BENCH_BUS_BIASED_COUNTS 688 /* bus under its bias, no match     */
#define MK1C_BENCH_CH_BIASED_COUNTS 1262 /* channel under its bias, no match */
#define MK1C_BENCH_C_BUS_NF 1100         /* C115 and the bus's strays        */
/* U9's OUT, off, conducting back into the part: a junction behind a resistance. */
#define MK1C_U9_REV_IS_A 1.2e-12
#define MK1C_U9_REV_NVT_V 0.040
#define MK1C_U9_REV_OHM 250
/* Each bias source: a 3.3 V GPIO through a BAT54WS. */
#define MK1C_BIAS_GPIO_V 3.3
#define MK1C_BIAS_DIODE_IS_A 3.7e-6
#define MK1C_BIAS_DIODE_NVT_V 0.045

/* ── Levels ──────────────────────────────────────────────────────── */

#define COLD_NODE_MAX_COUNTS 50   /* an unbiased node with nothing on it   */
#define TRACK_BUS_MIN_COUNTS 200u /* a biased bus below this did not rise */

/* ── The presence verdict ────────────────────────────────────────── */

typedef enum { TRACK_OPEN, TRACK_PRESENT, TRACK_INVALID } track_t;

static inline track_t track_channel(uint16_t channel, uint16_t bus) {
    if (bus < TRACK_BUS_MIN_COUNTS)
        return TRACK_INVALID;
    return (2u * (uint32_t)channel >= bus) ? TRACK_PRESENT : TRACK_OPEN;
}

#endif /* PYRO_SENSE_H */
