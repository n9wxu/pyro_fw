/*
 * Plant — Pyro MK1B.
 *
 * Nothing in the repository describes this board's sense network, so the
 * model was built from the schematic rather than from a comment. Exported
 * with
 *     kicad-cli sch export netlist --format kicadsexpr \
 *         ~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch
 * and read off the nets:
 *
 *   +3V3 ─[R26 100k]─┬── Switched_BAT1 ──[CN1.1] igniter 1 [CN1.2]──┐
 *                    │      │                                       │
 *                 U5 OUT2  [R25 100R]─┬─ SENSE1 (ADC0)              │
 *                (AP2192)             └─ C25 100nF ─ GND            │
 *                                                                   ├─ Net-(CN1-Pad2)
 *   +3V3 ─[R19 100k]─┬── Switched_BAT2 ──[CN1.4] igniter 2 [CN1.3]──┘
 *                    │      │                                       │
 *                 U5 OUT1  [R18 100R]─┬─ SENSE2 (ADC1)              │
 *                (AP2192)             └─ C21 100nF ─ GND            │
 *                                                                   │
 *              Net-(CN1-Pad2) ──[F2 polyfuse]── Q1B (AO6800) ── GND ┘
 *                                                gate = sw_gnd = GPIO15
 *
 * ── Two things the netlist settles ───────────────────────────────
 *
 * 1. PYRO_COMMON_EN (GPIO15) is the SHARED LOW-SIDE GATE, not a high-side
 *    enable. The net is sw_gnd and it lands on Q1 pin 3, the gate of the
 *    AO6800's second FET, whose drain goes through F2 to the igniters'
 *    common return. The name in board_pins.h reads as though it enables
 *    something on the high side; it does not. MK1A's file has this right
 *    in prose -- "the same two-key property MK1B gets from PYRO_COMMON_EN,
 *    with the shared element moved to the low side" -- and the two boards
 *    turn out to have the SAME sense topology, not mirrored ones.
 *
 * 2. The pull-up is 100 kohm (R26/R19) to +3V3, exactly like MK1A's
 *    R9/R10, with a 100 ohm series resistor (R25/R18) and a 100 nF cap
 *    (C25/C21) at the pin. So the sense node behaves as MK1A's does, and
 *    the two numbers that follow are the ones the firmware has to live
 *    with:
 *
 *      a present igniter pulls the node to nearly 0 V   -> about 0 counts
 *      an open channel charges through 100.1k x 100nF   -> tau 10.0 ms
 *
 * The AP2192's enables (GPIO21 -> EN2 -> OUT2 -> channel 1, GPIO22 -> EN1
 * -> OUT1 -> channel 2) are modelled active-high, which is what the
 * firmware assumes when it writes 0 to keep them off and 1 to fire.
 *
 * ── The FLAG pins ────────────────────────────────────────────────
 *
 * U5's FLG2 and FLG1 are open drain with 100 kohm pull-ups (R21/R20) and
 * land on GPIO17 and GPIO18. MK1B is the only one of the three boards with
 * a real fault output, so it is the only one whose gpio_in hook does
 * anything. The current at which the AP2192 asserts it is *datasheet*: the
 * value below has not been checked against the part's own limit, and no
 * firmware behaviour depends on its exact value -- only on the pin going
 * low at all.
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
#define R_SENSE_OHM      100.0   /* R25 / R18                            */
#define C_SENSE_F       100e-9   /* C25 / C21                            */
#define V_LOGIC            3.3
#define R_FUSE_OHM        0.05   /* F2, BSMD0805L-150 polyfuse           */
/* An "off" high side is an ideal open by default; see
 * plant_set_highside_leak_ohms(). */
#define AP2192_ILIM_A      2.0   /* *datasheet*, see the note above      */

static double highside_ohms(const plant_t *p, int ch) {
    int pin = (ch == 0) ? BOARD_PIN_PYRO1_EN : BOARD_PIN_PYRO2_EN;
    if (p->faults[PF_HIGH_SIDE_SHORT])
        return 0.10;
    if (plant_gpio(p, pin))
        return 0.10;
    return (p->highside_leak_ohms > 0.0) ? p->highside_leak_ohms : 1e11;
}

static double lowside_ohms(const plant_t *p) {
    bool shorted = p->faults[PF_LOWSIDE_A_SHORT] || p->faults[PF_LOWSIDE_B_SHORT];
    if (shorted)
        return 0.03 + R_FUSE_OHM;
    return plant_gpio(p, BOARD_PIN_PYRO_COMMON_EN) ? (0.03 + R_FUSE_OHM) : 1e11;
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

    net_cap_to_gnd(n, N_COMMON, 1e-9);
    net_res_to_gnd(n, N_COMMON, lowside_ohms(p));
    if (p->faults[PF_BUS_SHORT_GND])
        net_res_to_gnd(n, N_COMMON, 0.5);

    for (int i = 0; i < 2; i++) {
        net_src_through_res(n, node[i], V_LOGIC, R_PULLUP_OHM);
        /* R25/R18 are 0.1 % of the pull-up, so the sense pin and the
         * switched-BAT node are the same node to within a tenth of a
         * percent and C25/C21 belong on it: tau = 100.1k x 100 nF. */
        net_cap_to_gnd(n, node[i], C_SENSE_F);
        net_src_through_res(n, node[i], plant_pack_v(p), highside_ohms(p, i));
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
        p->pack_sag_v = i * 0.15;
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
