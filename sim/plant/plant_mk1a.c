/*
 * Plant: MK1A's firing network, a high side per channel and one shared low
 * side, sensed with no current from the pack. See
 * boards/mk1a/THEORY_OF_OPERATION.md "Pyro circuit" and "Continuity check":
 * its count table and its 10.1 ms open-channel rise are what
 * test/test_plant.c holds this model to. The ADC clamp current a fire pulse
 * drives through R5/R14 is not modelled; only the clamped reading is.
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
#define C_SENSE_F        100e-9   /* C6 / C5                             */
#define V_LOGIC             3.3

/* The 8 A fuse and the shared low-side FET. The fuse is modelled as a
 * plain resistance: nothing in the firmware can read it, and a blown fuse
 * is indistinguishable from an open low side, which PF_BUS_SHORT_GND's
 * opposite already covers. */
#define R_FUSE_OHM          0.01
#define DMC2053_ON_OHM      0.05   /* Q6 / Q1, the high sides            */

static double highside_ohms(const plant_t *p, int ch) {
    int pin = (ch == 0) ? BOARD_PIN_FIRE1 : BOARD_PIN_FIRE2;
    if (p->faults[PF_HIGH_SIDE_SHORT])
        return DMC2053_ON_OHM;
    if (plant_gpio(p, pin))
        return DMC2053_ON_OHM;
    return (p->highside_leak_ohms > 0.0) ? p->highside_leak_ohms : PLANT_OPEN_OHM;
}

static double lowside_ohms(const plant_t *p) {
    /* PF_LOWSIDE_A_SHORT stands for the single shared Q2 on this board:
     * there is only one low side, so both channel faults map to it and the
     * A one is the canonical name. */
    bool shorted = p->faults[PF_LOWSIDE_A_SHORT] || p->faults[PF_LOWSIDE_B_SHORT];
    if (shorted)
        return PLANT_FET_ON_OHM + R_FUSE_OHM;
    return plant_gpio(p, BOARD_PIN_PYRO_LOW) ? (PLANT_FET_ON_OHM + R_FUSE_OHM) : PLANT_OPEN_OHM;
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

    /* With PYRO_LOW released the shared low node floats: it has no
     * pull-down, which is what makes the shorts step a test. */
    net_cap_to_gnd(n, N_COMMON, PLANT_STRAY_F);
    net_res_to_gnd(n, N_COMMON, lowside_ohms(p));
    if (p->faults[PF_BUS_SHORT_GND])
        net_res_to_gnd(n, N_COMMON, PLANT_HARD_SHORT_OHM);

    for (int i = 0; i < 2; i++) {
        net_src_through_res(n, node[i], V_LOGIC, R_PULLUP_OHM);

        /* C6/C5 sit behind R5/R14, 1 % of the pull-up: on the node to 1 %. */
        net_cap_to_gnd(n, node[i], C_SENSE_F);

        net_src_through_res(n, node[i], plant_pack_v(p), highside_ohms(p, i));

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
        p->pack_sag_v = i * PLANT_PACK_SAG_OHM;
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
