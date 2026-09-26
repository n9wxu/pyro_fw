/*
 * Plant — Pyro MK1C.
 *
 * Models the firing network of ~/Documents/pyro_mk1c/DESIGN.md: a
 * TPS259570 eFuse on the high side driven by a software charge pump, two
 * low-side channel FETs, three switched bias injectors, and four divided
 * sense taps.
 *
 *   3.0V ─[330]─┬─────────── FIRING BUS (node 0) ──┬─ C_BULK
 *   (BIAS_BUS)  │                                  ├─ R_BLEED 2.2k ─ GND
 *          U9 ──┘                                  └─ divider 14.99k ─ GND
 *          eFuse                │            │
 *          from VBAT       [match A]    [match B]     (TVS in parallel)
 *                               │            │
 *                          node 1 (A)   node 2 (B)
 *                               ├─ divider 14.99k ─ GND
 *                               ├─ Q103 ─ GND   (gate = FIRE_A)
 *                               └─ [330] ─ 3.0V (BIAS_A)
 *
 * ── Why the levels come out right ────────────────────────────────
 *
 * DESIGN.md 4 tabulates three levels and boards/mk1c/pyro_board.c asserts
 * all three at compile time. They are the model's acceptance test, and all
 * three fall out of the divider algebra above:
 *
 *   bus under its own bias, no match
 *       Rpd = 14.99k || 2.2k = 1918 ohm
 *       3.0 x 1918/(330+1918)      = 2.560 V = 1058 counts
 *   channel under its own bias, match disconnected
 *       3.0 x 14990/(330+14990)    = 2.935 V = 1212 counts   (DESIGN: 1214)
 *   channel tied to the bus through a match
 *       14.99k || (1 + 1918)       = 1701 ohm
 *       3.0 x 1701/(330+1701)      = 2.513 V = 1038 counts   (DESIGN: 1037)
 *
 * test/test_plant_mk1c.c asserts these against the firmware's own
 * constants, so a change to either side that breaks the agreement fails
 * rather than quietly shifting a threshold.
 *
 * ── Two constants that need naming ───────────────────────────────
 *
 * R_BLEED: DESIGN.md is inconsistent with itself and with the board. The
 * architecture sketch in section 2 says 2.2 k; the component table in
 * section 3 says "2 x 4.7 kohm in parallel", which is 2.35 k, and argues
 * for the split on the grounds that one open resistor must not defeat the
 * only discharge path. The board as drawn has a SINGLE R103 of 2.2 k --
 * verified by pulling the reference-to-value map out of
 * ~/Documents/pyro_mk1c/pyro.kicad_sch, whose pyro sheet is R100-R122 plus
 * R130 and contains exactly one 2.2 k part. The model follows the board.
 *
 * The bus pull-down is therefore 14.99k || 2.2k = 1918 ohm, which is what
 * pyro_board.c's design_rpd_ohm says and what DESIGN.md 4's formulas use
 * ("3.0 x 1.92/(0.33 + 1.92)"). DESIGN.md 4's own pull-down row disagrees
 * with its formulas: it says 1.85 kohm and tells the reader to
 * include "the 16.7 kohm indicator divider" from section S9. No such
 * divider is placed on the pyro sheet -- there is no NPN sink, no BAV99
 * instance and no 12k/150R pair anywhere in the reference map -- so the S9
 * indicator load does not exist on this revision and 1.92 kohm is right.
 *
 * ── The board as measured (DD-054) ──────────────────────────────
 *
 * The bench MK1C, 2026-09-26, its ADC and a scope at CN1: the bus sits at
 * about 688 counts under bias, not the 1058 the divider algebra above gives.
 * R120, R103 and C115 are as designed -- the rise starts at 9 mA into about
 * 1 uF, and below 0.72 V the bus decays with the designed pull-down's time
 * constant -- but above about 0.72 V U9's OUT conducts back into the part,
 * a junction with about 250 ohm behind it, 2.4 mA at 1.5 V. It is expected
 * of the part and is modelled, not fixed. U9_REV_* fits the scope's points
 * within about 10 mV.
 *
 * The bias sources are a 3.3 V GPIO through a BAT54WS (D104/D106/D107), not
 * a fixed 3.0 V: 3.12 V at a channel's 0.2 mA and 2.99 V at the bus's 4 mA,
 * which is what puts the isolated channel at 1262 counts rather than 1212.
 *
 * Both are nonlinear and the network is solved linearly, so each step
 * stamps them linearized about the previous step's node voltage.
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant_internal.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>

/* Node indices. plant_probe() reports node 0 as the bus. */
#define N_BUS 0
#define N_A   1
#define N_B   2

/* ── Component values (DESIGN.md 3) ──────────────────────────────── */

#define R_BIAS_OHM       330.0    /* R112 / R117 / R120                   */
#define R_DIV_OHM       14990.0   /* 10k + 4.99k, the whole divider chain */
#define R_DIV_RATIO     (4990.0 / 14990.0)  /* 0.33289                    */
#define R_BLEED_OHM      2200.0   /* see the note above                   */
#define R_VBAT_DIV_OHM 149900.0   /* 100k + 49.9k                         */
#define R_VBAT_RATIO    (49900.0 / 149900.0)
#define GPIO_HIGH_V         3.3
/* BAT54WS, fitted to the bench: 3.12 V at 0.2 mA, 2.99 V at 4 mA. */
#define BIAS_DIODE_IS_A    3.7e-6
#define BIAS_DIODE_NVT_V   0.045
/* U9's OUT, off, conducting back into the part: fitted to the scope. */
#define U9_REV_IS_A        1.2e-12
#define U9_REV_NVT_V       0.040
#define U9_REV_OHM         250.0
#define C_ADC_F           10e-9
#define C_NODE_F           1e-9   /* drain node strays: TVS, FET, track    */

/* The tap filter DESIGN.md 4 calls "the 33 us ADC filter":
 * (10k || 4.99k) x 10 nF = 3.33k x 10 nF. */
#define TAP_TAU_S ((10000.0 * 4990.0 / 14990.0) * C_ADC_F)
#define VBAT_TAP_TAU_S ((100000.0 * 49900.0 / 149900.0) * C_ADC_F)

/* ── U9, TPS259570 (DESIGN.md 3) ─────────────────────────────────── */

#define EFUSE_RDSON_OHM   0.034
#define EFUSE_ILIM_A      4.05    /* R_ILIM 499 ohm                       */
#define EFUSE_SLEW_V_PER_S 890.0  /* C_dVdT 47 nF -> 0.89 V/ms            */
#define EFUSE_EN_ON_V     1.20    /* EN/UVLO rising threshold             */
#define EFUSE_EN_OFF_V    1.10
/* How long U9 may sit in current limit before it latches off. The part is
 * the latch-off type, and DESIGN.md 3 shows a correct precharge never
 * reaches ILIM (2.0 A at 2200 uF against a 4.05 A limit), so this only
 * fires on a real fault. The value itself is *bench*: it stands in for the
 * thermal element and has not been measured. */
#define EFUSE_ILIM_LATCH_S 0.010

/* ── The charge pump (DESIGN.md 5.1) ─────────────────────────────── */

#define C_PUMP_F      10e-9
#define C_HOLD_F     100e-9
#define R_BLEED_EN_OHM 100000.0
#define PUMP_DIODE_V     0.30     /* BAT54S, two drops in the transfer    */

/* ── The mechanical arm disconnect ───────────────────────────────── */

/* Two poles, breaking both match leads. Closed unless PF_ARM_SWITCH_OPEN.
 * DESIGN.md 5.3 budgets 0.2 ohm for the whole firing loop, of which the
 * switch and the harness are most; 0.1 ohm here leaves room for the leads. */
#define R_ARM_CLOSED_OHM 0.10

/* A junction in series with a resistance: V = nvt ln(1 + I/is) + r I. The
 * current at v, by bisection on the monotonic inverse, and dI/dV there. */
static double junction_i(double v, double is, double nvt, double r, double *g) {
    if (v <= 0.0) {
        *g = is / nvt; /* reverse: its leakage, near enough nothing */
        return 0.0;
    }
    double lo = 0.0, hi = v / r;
    for (int k = 0; k < 60; k++) {
        double mid = 0.5 * (lo + hi);
        if (nvt * log1p(mid / is) + r * mid < v)
            lo = mid;
        else
            hi = mid;
    }
    double i = 0.5 * (lo + hi);
    *g = 1.0 / (nvt / (i + is) + r);
    return i;
}

/* A bias GPIO, high, through its Schottky and 330 ohm into a node: the
 * current it drives, linearized about the node's last voltage. */
static void stamp_bias(net_t *n, int node) {
    double v0 = n->v[node], g;
    double i0 = junction_i(GPIO_HIGH_V - v0, BIAS_DIODE_IS_A, BIAS_DIODE_NVT_V, R_BIAS_OHM, &g);
    net_res_to_gnd(n, node, 1.0 / g);
    n->i[node] += i0 + g * v0;
}

/* U9's reverse path from the bus to ground, linearized the same way. */
static void stamp_u9_reverse(net_t *n) {
    double v0 = n->v[N_BUS], g;
    double i0 = junction_i(v0, U9_REV_IS_A, U9_REV_NVT_V, U9_REV_OHM, &g);
    net_res_to_gnd(n, N_BUS, 1.0 / g);
    n->i[N_BUS] -= i0 - g * v0;
}

static double arm_path_ohms(const plant_t *p) {
    return p->faults[PF_ARM_SWITCH_OPEN] ? 1e11 : R_ARM_CLOSED_OHM;
}

/* Match A or B in circuit, between the bus and its drain node: the
 * bridgewire, the mechanical disconnect, and the TVS across the connector
 * in parallel with the lot.
 *
 * A shorted TVS and a connected match are the same reading -- DESIGN.md
 * calls this out as the reason T3 alone cannot separate them -- and the
 * model reproduces that by construction rather than by a special case. */
static double channel_path_ohms(const plant_t *p, int idx) {
    double r = plant_match_ohms(&p->match[idx]) + arm_path_ohms(p);
    plant_fault_t tvs = (idx == 0) ? PF_TVS_A_SHORT : PF_TVS_B_SHORT;
    if (p->faults[tvs]) {
        /* The TVS is at the connector, on the board side of the mechanical
         * disconnect, so a shorted one conducts even with the arm out. */
        double rt = 1.0;
        r = (r * rt) / (r + rt);
    }
    return r;
}

/* ── eFuse and pump, stepped outside the network ─────────────────── */

static void pump_and_efuse(plant_t *p, double dt_s) {
    bool arm = plant_gpio(p, BOARD_PIN_ARM_TOGGLE);

    /* Each rising edge moves charge from C_PUMP into C_HOLD. The split is
     * the capacitive divider between them, less the diode drop, which is
     * why the pump converges on roughly 3.3 - 2 x Vf rather than on 3.3. */
    if (arm && !p->arm_prev) {
        double top = 3.3 - 2.0 * PUMP_DIODE_V;
        double delta = (top - p->en_hold_v) * (C_PUMP_F / (C_PUMP_F + C_HOLD_F));
        if (delta > 0.0)
            p->en_hold_v += delta;
    }
    p->arm_prev = arm;

    /* R_BLEED_EN drains C_HOLD whenever the pump is not keeping up. This is
     * the disarm path: stopping the toggle IS the disarm. */
    double tau = R_BLEED_EN_OHM * C_HOLD_F;
    p->en_hold_v -= p->en_hold_v * (dt_s / (tau + dt_s));
    if (p->en_hold_v < 0.0)
        p->en_hold_v = 0.0;

    bool want_on;
    if (p->efuse_latched || p->faults[PF_EFUSE_LATCHED])
        want_on = false;
    else if (p->efuse_on)
        want_on = p->en_hold_v > EFUSE_EN_OFF_V;
    else
        want_on = p->en_hold_v > EFUSE_EN_ON_V;

    /* A high side that will not turn off. The single failure DESIGN.md 8.1
     * looks for with its quiescent bus read. */
    if (p->faults[PF_EFUSE_WONT_TURN_OFF] && p->efuse_on)
        want_on = true;

    if (want_on && !p->efuse_on)
        p->bus_ramp_v = p->net.v[N_BUS]; /* the ramp starts where the bus is */
    p->efuse_on = want_on;

    if (p->efuse_on) {
        double pack = plant_pack_v(p);
        p->bus_ramp_v += EFUSE_SLEW_V_PER_S * dt_s;
        if (p->bus_ramp_v > pack)
            p->bus_ramp_v = pack;
    } else {
        p->bus_ramp_v = 0.0;
    }
}

/* ── Network ─────────────────────────────────────────────────────── */

/* Set by post() and used by the next build() to decide whether U9 is in
 * its voltage-limited ramp or in current limit. One step of lag, which at
 * 50 us is far inside the dVdT ramp it has to track. */
static double efuse_i_prev;
static double efuse_ilim_run_s;

static void mk1c_reset(plant_t *p) {
    (void)p;
    efuse_i_prev = 0.0;
    efuse_ilim_run_s = 0.0;
}

static double mk1c_max_dt(plant_t *p) {
    /* Fine while the pump or the firing path is live; coarse when the
     * board is just sitting on the pad with everything cold. */
    if (p->efuse_on || plant_gpio(p, BOARD_PIN_ARM_TOGGLE) ||
        plant_gpio(p, BOARD_PIN_FIRE_A) || plant_gpio(p, BOARD_PIN_FIRE_B))
        return 25e-6;
    if (plant_gpio(p, BOARD_PIN_BIAS_BUS) || plant_gpio(p, BOARD_PIN_BIAS_A) ||
        plant_gpio(p, BOARD_PIN_BIAS_B))
        return 50e-6;
    return 500e-6;
}

static void mk1c_build(plant_t *p, double dt_s) {
    net_t *n = &p->net;

    pump_and_efuse(p, dt_s);

    /* ── Bus ── */
    if (p->bus_pulldown_override > 0.0) {
        /* A measured pull-down replaces the whole design network: divider
         * and bleed together, because a bench decay constant cannot say
         * which of the two is wrong. */
        net_res_to_gnd(n, N_BUS, p->bus_pulldown_override);
    } else {
        net_res_to_gnd(n, N_BUS, R_DIV_OHM);
        if (!p->faults[PF_BLEED_OPEN])
            net_res_to_gnd(n, N_BUS, R_BLEED_OHM);
    }
    net_cap_to_gnd(n, N_BUS, p->c_bulk_f);
    if (plant_gpio(p, BOARD_PIN_BIAS_BUS) && !p->faults[PF_BIAS_BUS_OPEN])
        stamp_bias(n, N_BUS);
    if (!p->efuse_on && !p->faults[PF_HIGH_SIDE_SHORT])
        stamp_u9_reverse(n);
    if (p->faults[PF_BUS_SHORT_GND])
        net_res_to_gnd(n, N_BUS, 0.5);

    if (p->faults[PF_HIGH_SIDE_SHORT]) {
        /* U9 shorted drain to source: the bus sits at the pack whatever the
         * pump does. This is the reading evaluate_faults() latches on. */
        net_src_through_res(n, N_BUS, plant_pack_v(p), EFUSE_RDSON_OHM);
    } else if (p->efuse_on) {
        if (fabs(efuse_i_prev) > EFUSE_ILIM_A) {
            n->i[N_BUS] += EFUSE_ILIM_A; /* current limit */
        } else {
            net_src_through_res(n, N_BUS, p->bus_ramp_v, EFUSE_RDSON_OHM);
        }
    }

    /* ── Channels ── */
    const int node[2] = {N_A, N_B};
    const int bias_pin[2] = {BOARD_PIN_BIAS_A, BOARD_PIN_BIAS_B};
    const int fire_pin[2] = {BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B};
    const plant_fault_t bias_open[2] = {PF_BIAS_A_OPEN, PF_BIAS_B_OPEN};
    const plant_fault_t fet_short[2] = {PF_LOWSIDE_A_SHORT, PF_LOWSIDE_B_SHORT};

    for (int i = 0; i < 2; i++) {
        net_res_to_gnd(n, node[i], R_DIV_OHM);
        net_cap_to_gnd(n, node[i], C_NODE_F);
        net_res_to_gnd(n, node[i],
                       plant_fet_ohms(p, plant_gpio(p, fire_pin[i]), fet_short[i]));
        if (plant_gpio(p, bias_pin[i]) && !p->faults[bias_open[i]])
            stamp_bias(n, node[i]);
        net_res(n, N_BUS, node[i], channel_path_ohms(p, i));
    }
}

static void mk1c_post(plant_t *p, double dt_s) {
    net_t *n = &p->net;

    p->i_a = net_current(n, N_BUS, N_A, channel_path_ohms(p, 0));
    p->i_b = net_current(n, N_BUS, N_B, channel_path_ohms(p, 1));

    /* What U9 is actually supplying: everything leaving the bus node, which
     * is the divider, the bleed, both channel paths and whatever the bulk
     * capacitor is taking. */
    double i_load = 0.0;
    if (p->bus_pulldown_override > 0.0) {
        i_load += net_current_to_gnd(n, N_BUS, p->bus_pulldown_override);
    } else {
        i_load += net_current_to_gnd(n, N_BUS, R_DIV_OHM);
        if (!p->faults[PF_BLEED_OPEN])
            i_load += net_current_to_gnd(n, N_BUS, R_BLEED_OHM);
    }
    if (p->faults[PF_BUS_SHORT_GND])
        i_load += net_current_to_gnd(n, N_BUS, 0.5);
    i_load += p->i_a + p->i_b;
    i_load += p->c_bulk_f * EFUSE_SLEW_V_PER_S * (p->efuse_on ? 1.0 : 0.0);
    efuse_i_prev = p->efuse_on ? i_load : 0.0;

    if (p->efuse_on && fabs(efuse_i_prev) > EFUSE_ILIM_A) {
        efuse_ilim_run_s += dt_s;
        if (efuse_ilim_run_s >= EFUSE_ILIM_LATCH_S) {
            p->efuse_latched = true; /* latch-off part: firmware must retry */
            p->efuse_on = false;
        }
    } else {
        efuse_ilim_run_s = 0.0;
    }

    /* Pack sag, when asked for. A pulse of several amps out of a small LiPo
     * through its internal resistance; DESIGN.md does not budget this, so
     * the value is deliberately crude and only exists to let a test ask
     * what the firmware does when VBAT moves during a fire. */
    if (p->faults[PF_PACK_COLLAPSE]) {
        double i = fabs(p->i_a) + fabs(p->i_b);
        p->pack_sag_v = i * 0.15;
    } else {
        p->pack_sag_v = 0.0;
    }
}

static adc_tap_t mk1c_adc_tap(int ch) {
    adc_tap_t t = {0};
    switch (ch) {
    case BOARD_ADC_CH_VBAT:
        /* SNS_VBAT taps the pack, not a solved node. Reported through the
         * same lag as the others so a test that watches it during a sag
         * sees the filter, not a step. */
        t.valid = true; t.node = -1; t.ratio = R_VBAT_RATIO; t.tau_s = VBAT_TAP_TAU_S;
        break;
    case BOARD_ADC_CH_BUS:
        t.valid = true; t.node = N_BUS; t.ratio = R_DIV_RATIO; t.tau_s = TAP_TAU_S; break;
    case BOARD_ADC_CH_A:
        t.valid = true; t.node = N_A;   t.ratio = R_DIV_RATIO; t.tau_s = TAP_TAU_S; break;
    case BOARD_ADC_CH_B:
        t.valid = true; t.node = N_B;   t.ratio = R_DIV_RATIO; t.tau_s = TAP_TAU_S; break;
    default:
        break;
    }
    return t;
}

static const plant_ops_t ops = {
    .name = "Pyro MK1C",
    .n_nodes = 3,
    .reset = mk1c_reset,
    .build = mk1c_build,
    .post = mk1c_post,
    .adc_tap = mk1c_adc_tap,
    .max_dt = mk1c_max_dt,
    .gpio_in = NULL, /* nothing on MK1C is an input: U9's ~FLT is not routed */
};

PLANT_REGISTER(PLANT_MK1C, &ops)
