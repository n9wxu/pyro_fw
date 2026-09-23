/*
 * Plant — Pyro MK1A.
 *
 * A different firing architecture from MK1C, and the difference is the
 * whole point of modelling it separately: MK1A has a per-channel HIGH side
 * and ONE shared LOW side, where MK1C has a shared high side and
 * per-channel low sides. Continuity is sensed with no current from the
 * pack at all.
 *
 *          +3V3 ─[R9 100k]─┬── node 1 (igniter 1 high) ──[J3 igniter]──┐
 *                          └─[R5 1k]─ SNS1 (ADC0), C6 100nF            │
 *   VBATT ─[Q6 DMC2053]────┘                                           │
 *                                                                      ├─ node 0
 *          +3V3 ─[R10 100k]─┬── node 2 (igniter 2 high) ──[J4 igniter]─┤  Initiator
 *                           └─[R14 1k]─ SNS2 (ADC1), C5 100nF          │  ground
 *   VBATT ─[Q1 DMC2053]─────┘                                          │
 *                                                                      │
 *                          node 0 ──[F1 8A]──[Q2 AO3400A]── GND        ┘
 *                                             gate = PYRO_LOW
 *
 * ── What the model has to get right ──────────────────────────────
 *
 * boards/mk1a/pyro_board.c states the numbers it expects, and they are the
 * acceptance test:
 *
 *   "against 100k, at 12 bits: a 2 ohm igniter reads 0 counts, a 1k bad
 *    joint 41, a 10k leakage path 372, and a genuine open 4095"
 *
 * and the timing that makes the sense cycle a state machine rather than
 * two sleeps:
 *
 *   "The node going OPEN has to charge C6/C5 (100nF) through R9+R5
 *    (101k). That is a 10.1 ms time constant, so an open channel needs
 *    about 50 ms to read as open."
 *
 * Both fall out of the network. The 100 nF sits on the sense node as node
 * capacitance, so the rise really is 101k x 100nF and firmware that
 * samples before SETTLE_MS reads a partly charged node here exactly as it
 * would on the bench. That is the single most valuable thing this model
 * does for MK1A: the 50 ms settle is a firmware constant justified by an
 * RC, and now the RC is in the loop.
 *
 * ── The sense node during a fire pulse ───────────────────────────
 *
 * board_pins.h warns that a fire pulse puts VBATT on the sense node
 * through R5/R14, "about 5 mA into the RP2040's ADC clamp for the 500 ms
 * of the pulse". The model reproduces the voltage and lets the ADC clamp
 * at full scale; it does not model the clamp current, because nothing in
 * the firmware can see it. The warning stands.
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant_internal.h"
#include "board_pins.h"

#include <math.h>
#include <stddef.h>

/* Node indices. Node 0 is the shared low side, which plant_probe() reports
 * as the bus -- it is the node this board shares between channels. */
#define N_COMMON 0
#define N_CH1    1
#define N_CH2    2

#define R_PULLUP_OHM   100000.0   /* R9 / R10, to +3V3                   */
#define R_SENSE_OHM      1000.0   /* R5 / R14, the ADC series resistor   */
#define C_SENSE_F        100e-9   /* C6 / C5                             */
#define V_LOGIC             3.3

/* The 8 A fuse and the shared low-side FET. The fuse is modelled as a
 * plain resistance: nothing in the firmware can read it, and a blown fuse
 * is indistinguishable from an open low side, which PF_BUS_SHORT_GND's
 * opposite already covers. */
#define R_FUSE_OHM          0.01

/* An "off" high side is an ideal open by default. A real one leaks, and on
 * this board the 100 kohm pull-up makes that visible -- see
 * plant_set_highside_leak_ohms(), which a test sets when it wants to ask. */

static double highside_ohms(const plant_t *p, int ch) {
    int pin = (ch == 0) ? BOARD_PIN_FIRE1 : BOARD_PIN_FIRE2;
    if (p->faults[PF_HIGH_SIDE_SHORT])
        return 0.05;
    if (plant_gpio(p, pin))
        return 0.05;
    return (p->highside_leak_ohms > 0.0) ? p->highside_leak_ohms : 1e11;
}

static double lowside_ohms(const plant_t *p) {
    /* PF_LOWSIDE_A_SHORT stands for the single shared Q2 on this board:
     * there is only one low side, so both channel faults map to it and the
     * A one is the canonical name. */
    bool shorted = p->faults[PF_LOWSIDE_A_SHORT] || p->faults[PF_LOWSIDE_B_SHORT];
    if (shorted)
        return 0.03 + R_FUSE_OHM;
    return plant_gpio(p, BOARD_PIN_PYRO_LOW) ? (0.03 + R_FUSE_OHM) : 1e11;
}

static double mk1a_max_dt(plant_t *p) {
    if (plant_gpio(p, BOARD_PIN_FIRE1) || plant_gpio(p, BOARD_PIN_FIRE2))
        return 25e-6;
    /* The slow edge is 10.1 ms. A 200 us step resolves it to half a
     * percent, and nothing else on this board is faster. */
    return 200e-6;
}

static void mk1a_build(plant_t *p, double dt_s) {
    net_t *n = &p->net;
    (void)dt_s;

    const int node[2] = {N_CH1, N_CH2};

    /* The shared low node. When PYRO_LOW is released it is genuinely
     * floating -- there is no pull-down on it at all -- which is what makes
     * phase 2 of the sense cycle a short test: the pull-ups should win on
     * both channels, and anything that still reads low is a short to
     * ground. Give it a small stray capacitance so it is a solvable node
     * rather than a singular one. */
    net_cap_to_gnd(n, N_COMMON, 1e-9);
    net_res_to_gnd(n, N_COMMON, lowside_ohms(p));
    if (p->faults[PF_BUS_SHORT_GND])
        net_res_to_gnd(n, N_COMMON, 0.5);

    for (int i = 0; i < 2; i++) {
        /* R9/R10 pull to +3V3, which as a Norton is a 3.3 V source behind
         * 100k -- a conductance to ground plus an injected current. This
         * is the element the whole sense scheme rests on: it is weak
         * enough that a 2 ohm igniter reads 0 counts and a 10k leakage
         * path still reads only 372, so a degraded connection lands in the
         * gap between the thresholds instead of rounding to "good". */
        net_src_through_res(n, node[i], V_LOGIC, R_PULLUP_OHM);

        /* The ADC filter cap hangs on the node through R5/R14. R5 is 1 % of
         * the pull-up, so the dominant constant is (R_PULLUP + R_SENSE) x
         * C_SENSE -- the 10.1 ms the firmware sizes SETTLE_MS against. */
        net_cap_to_gnd(n, node[i], C_SENSE_F);

        /* The high side, from the pack. */
        net_src_through_res(n, node[i], plant_pack_v(p), highside_ohms(p, i));

        /* The igniter, from this node down to the shared low node. */
        net_res(n, node[i], N_COMMON, plant_match_ohms(&p->match[i]));
    }
}

static void mk1a_post(plant_t *p, double dt_s) {
    (void)dt_s;
    net_t *n = &p->net;
    p->i_a = net_current(n, N_CH1, N_COMMON, plant_match_ohms(&p->match[0]));
    p->i_b = net_current(n, N_CH2, N_COMMON, plant_match_ohms(&p->match[1]));

    if (p->faults[PF_PACK_COLLAPSE]) {
        double i = fabs(p->i_a) + fabs(p->i_b);
        p->pack_sag_v = i * 0.15;
    } else {
        p->pack_sag_v = 0.0;
    }
}

static adc_tap_t mk1a_adc_tap(int ch) {
    adc_tap_t t = {0};
    /* The sense pin sits at the node: R5/R14 carry no current into the ADC
     * input, so there is no division, and C6/C5 are already the node's
     * capacitance. Both effects are in the network, so the tap is direct. */
    if (ch == BOARD_ADC_CH_SENSE1) { t.valid = true; t.node = N_CH1; t.ratio = 1.0; t.tau_s = 0.0; }
    else if (ch == BOARD_ADC_CH_SENSE2) { t.valid = true; t.node = N_CH2; t.ratio = 1.0; t.tau_s = 0.0; }
    return t;
}

static const plant_ops_t ops = {
    .name = "Pyro MK1A",
    .n_nodes = 3,
    .reset = NULL,
    .build = mk1a_build,
    .post = mk1a_post,
    .adc_tap = mk1a_adc_tap,
    .max_dt = mk1a_max_dt,
    .gpio_in = NULL, /* no FLAG or fault output exists on this board */
};

PLANT_REGISTER(PLANT_MK1A, &ops)
