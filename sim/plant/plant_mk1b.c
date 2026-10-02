/*
 * Plant: MK1B's sense and firing network, from its netlist (the KiCad
 * schematic pyro_mk1b.kicad_sch, on the author's machine and not in this
 * repository). See boards/mk1b/THEORY_OF_OPERATION.md "Pyro circuit".
 *
 * PYRO_COMMON_EN is the shared low-side gate (Q1B), not a high-side enable,
 * so the sense node is MK1A's: 100 kohm to 3V3, 100 ohm and 100 nF at the pin.
 * U5 is modelled as fitted, the AP2192A, whose disabled outputs discharge
 * the sense nodes; plant_set_mk1b_u5() selects the netlist's AP2192 (DD-059,
 * and boards/mk1b/THEORY_OF_OPERATION.md "Known limits").
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant_internal.h"
#include "board_pins.h"

#include <math.h>
#include <stddef.h>

#define N_COMMON 0
#define N_CH1    1
#define N_CH2    2

#define R_PULLUP_OHM  100000.0   /* R26 / R19                            */
#define C_SENSE_F       100e-9   /* C25 / C21                            */
#define V_LOGIC            3.3
#define R_FUSE_OHM        0.05   /* F2, BSMD0805L-150 polyfuse           */
#define AP2192_ON_OHM      0.10
/* The current FLAG asserts at: unchecked against DS32193, and nothing in the
 * firmware depends on its value, only on the pin going low. */
#define AP2192_ILIM_A      2.0
#define AP2192A_RDIS_OHM 100.0   /* DS32193 p.4, RDIS                    */

static double highside_ohms(const plant_t *p, int ch) {
    int pin = (ch == 0) ? BOARD_PIN_PYRO1_EN : BOARD_PIN_PYRO2_EN;
    if (p->faults[PF_HIGH_SIDE_SHORT])
        return AP2192_ON_OHM;
    if (plant_gpio(p, pin))
        return AP2192_ON_OHM;
    return (p->highside_leak_ohms > 0.0) ? p->highside_leak_ohms : PLANT_OPEN_OHM;
}

static double lowside_ohms(const plant_t *p) {
    bool shorted = p->faults[PF_LOWSIDE_A_SHORT] || p->faults[PF_LOWSIDE_B_SHORT];
    if (shorted)
        return PLANT_FET_ON_OHM + R_FUSE_OHM;
    return plant_gpio(p, BOARD_PIN_PYRO_COMMON_EN) ? (PLANT_FET_ON_OHM + R_FUSE_OHM) : PLANT_OPEN_OHM;
}

static double mk1b_max_dt(plant_t *p) {
    if (plant_gpio(p, BOARD_PIN_PYRO1_EN) || plant_gpio(p, BOARD_PIN_PYRO2_EN))
        return 25e-6;
    return 200e-6;
}

static void mk1b_build(plant_t *p, double dt_s) {
    net_t *n = &p->net;
    (void)dt_s;

    const int node[2] = {N_CH1, N_CH2};

    net_cap_to_gnd(n, N_COMMON, PLANT_STRAY_F);
    net_res_to_gnd(n, N_COMMON, lowside_ohms(p));
    if (p->faults[PF_BUS_SHORT_GND])
        net_res_to_gnd(n, N_COMMON, PLANT_HARD_SHORT_OHM);

    for (int i = 0; i < 2; i++) {
        net_src_through_res(n, node[i], V_LOGIC, R_PULLUP_OHM);
        /* R25/R18 are 0.1 % of the pull-up, so the sense pin and the
         * switched-BAT node are the same node to within a tenth of a
         * percent and C25/C21 belong on it: tau = 100.1k x 100 nF. */
        net_cap_to_gnd(n, node[i], C_SENSE_F);
        net_src_through_res(n, node[i], plant_pack_v(p), highside_ohms(p, i));
        int en_pin = (i == 0) ? BOARD_PIN_PYRO1_EN : BOARD_PIN_PYRO2_EN;
        if (p->mk1b_u5_discharges && !plant_gpio(p, en_pin))
            net_res_to_gnd(n, node[i], AP2192A_RDIS_OHM);
        net_res(n, node[i], N_COMMON, plant_match_ohms(&p->match[i]));
    }
}

static void mk1b_post(plant_t *p, double dt_s) {
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

static adc_tap_t mk1b_adc_tap(int ch) {
    adc_tap_t t = {0};
    if (ch == 0)      { t.valid = true; t.node = N_CH1; t.ratio = 1.0; t.tau_s = 0.0; }
    else if (ch == 1) { t.valid = true; t.node = N_CH2; t.ratio = 1.0; t.tau_s = 0.0; }
    return t;
}

/* FLG2 (GPIO17) belongs to OUT2, which is channel 1; FLG1 (GPIO18) to
 * OUT1, which is channel 2. The crossing is in the schematic, not a
 * mistake here. Open drain, active low, 100k pull-up. */
static bool mk1b_gpio_in(plant_t *p, int gpio, bool *level) {
    double i;
    if (gpio == BOARD_PIN_PYRO1_FLAG)      i = fabs(p->i_a);
    else if (gpio == BOARD_PIN_PYRO2_FLAG) i = fabs(p->i_b);
    else return false;

    int ch_pin = (gpio == BOARD_PIN_PYRO1_FLAG) ? BOARD_PIN_PYRO1_EN : BOARD_PIN_PYRO2_EN;
    bool fault = plant_gpio(p, ch_pin) && i > AP2192_ILIM_A;
    *level = !fault; /* pulled high when healthy */
    return true;
}

static const plant_ops_t ops = {
    .name = "Pyro MK1B",
    .n_nodes = 3,
    .reset = NULL,
    .build = mk1b_build,
    .post = mk1b_post,
    .adc_tap = mk1b_adc_tap,
    .max_dt = mk1b_max_dt,
    .gpio_in = mk1b_gpio_in,
};

PLANT_REGISTER(PLANT_MK1B, &ops)
