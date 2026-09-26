/*
 * Board plant — shared core. The board-specific networks are in
 * plant_mk1a.c, plant_mk1b.c and plant_mk1c.c; what lives here is the
 * dispatch, the GPIO and ADC plumbing, and the e-match, which is the same
 * device on all three boards.
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant_internal.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static plant_t g_plant;
static bool g_inited;

/* ── The e-match ─────────────────────────────────────────────────── */

void plant_match_defaults(plant_match_t *m) {
    memset(m, 0, sizeof(*m));
    m->state = MATCH_PRESENT;
    m->r_ohm = 1.0;            /* M1 typical  */
    m->leak_ohm = 0.0;
    m->no_fire_a = 0.100;      /* M2 minimum  */
    m->all_fire_a = 1.0;       /* M4 maximum  */
    m->all_fire_hold_s = 0.050;
    m->fire_energy_j = 0.015;  /* M5 maximum  */
    m->thermal_tau_s = 0.003;  /* M6 minimum  */
    m->spent_as = MATCH_SPENT_OPEN; /* M7 typical */
}

/* Terminal-to-terminal resistance, including any parallel leakage.
 *
 * Returns a very large number rather than an infinity for an open, so it
 * can go straight into net_res() without the caller branching. */
double plant_match_ohms(const plant_match_t *m) {
    double r;
    switch (m->state) {
    case MATCH_PRESENT:     r = (m->r_ohm > 0.0) ? m->r_ohm : 1.0; break;
    case MATCH_SHORT:       r = 1e-3; break;
    case MATCH_SPENT_SHORT: r = 1e-3; break;
    case MATCH_SPENT_OPEN:  r = 1e11; break;
    case MATCH_ABSENT:
    default:                r = 1e11; break;
    }
    if (m->leak_ohm > 0.0) {
        /* A dirty connector or a tracking path, in parallel with whatever
         * the match itself is doing. DESIGN.md calls out 5 kohm as the
         * value that passes a naive presence threshold. */
        r = (r * m->leak_ohm) / (r + m->leak_ohm);
    }
    return r;
}

/* Integrate one channel. Current comes from the board model, which is the
 * only thing that knows how its network delivers it. */
static void match_integrate(plant_t *p, int idx, double i_a, double dt_s) {
    plant_match_t *m = &p->match[idx];
    m->last_i_a = i_a;

    double ia = fabs(i_a);

    /* M3: how long this match has been held above its no-fire current. A
     * test asserts against the datasheet's duration qualifier; the model
     * only counts. */
    if (ia >= m->no_fire_a)
        m->no_fire_exposure_s += dt_s;

    if (m->fired) {
        /* Stop integrating, but KEEP energy_j: it is the record of how
         * much went into the bridgewire to light it, which is the number a
         * margin test against M5 wants. Zeroing it here destroyed the only
         * evidence the pulse ever produced. */
        m->all_fire_run_s = 0.0;
        return;
    }
    if (m->state != MATCH_PRESENT) {
        /* Nothing in circuit to heat. */
        m->all_fire_run_s = 0.0;
        m->energy_j = 0.0;
        return;
    }

    /* Energy criterion. The leak term is what makes a pulse longer than
     * thermal_tau_s cost more total energy than a short one -- the whole
     * argument in DESIGN.md 7.3 for firing from the capacitor rather than
     * from a current-limited supply. */
    double p_w = ia * ia * ((m->r_ohm > 0.0) ? m->r_ohm : 1.0);
    double tau = (m->thermal_tau_s > 0.0) ? m->thermal_tau_s : 0.003;
    m->energy_j += (p_w - m->energy_j / tau) * dt_s;
    if (m->energy_j < 0.0)
        m->energy_j = 0.0;

    /* Sustained-current criterion. */
    if (ia >= m->all_fire_a)
        m->all_fire_run_s += dt_s;
    else
        m->all_fire_run_s = 0.0;

    bool fire = (m->energy_j >= m->fire_energy_j) ||
                (m->all_fire_run_s >= m->all_fire_hold_s);

    if (fire) {
        m->fired = true;
        m->state = m->spent_as;
        p->fire_events++;
        p->last_fire_channel = idx + 1;
    }
}

/* ── FETs ────────────────────────────────────────────────────────── */

double plant_fet_ohms(const plant_t *p, bool gate_high, plant_fault_t short_fault) {
    if (short_fault < PF_COUNT && p->faults[short_fault])
        return 0.03; /* drain-source short: conducting whatever the gate does */
    return gate_high ? 0.03 : 1e11;
}

/* ── Pack ────────────────────────────────────────────────────────── */

double plant_pack_v(const plant_t *p) {
    double v = p->pack_mv / 1000.0 - p->pack_sag_v;
    return (v < 0.0) ? 0.0 : v;
}

/* ── Faults ──────────────────────────────────────────────────────── */

static const char *const fault_names[PF_COUNT] = {
    [PF_HIGH_SIDE_SHORT]      = "high-side short to pack",
    [PF_BUS_SHORT_GND]        = "firing bus shorted to ground",
    [PF_LOWSIDE_A_SHORT]      = "channel A low-side FET shorted",
    [PF_LOWSIDE_B_SHORT]      = "channel B low-side FET shorted",
    [PF_BIAS_A_OPEN]          = "channel A bias injector open",
    [PF_BIAS_B_OPEN]          = "channel B bias injector open",
    [PF_BIAS_BUS_OPEN]        = "bus bias injector open",
    [PF_BLEED_OPEN]           = "bleed resistor open",
    [PF_TVS_A_SHORT]          = "channel A TVS shorted",
    [PF_TVS_B_SHORT]          = "channel B TVS shorted",
    [PF_EFUSE_LATCHED]        = "eFuse latched off",
    [PF_EFUSE_WONT_TURN_OFF]  = "eFuse stuck on",
    [PF_ARM_SWITCH_OPEN]      = "mechanical arm disconnect open",
    [PF_PACK_COLLAPSE]        = "pack collapses under load",
};

const char *plant_fault_name(plant_fault_t f) {
    if (f < 0 || f >= PF_COUNT || !fault_names[f])
        return "?";
    return fault_names[f];
}

/* Which faults have an element to inject into on which board. A sweep over
 * the whole enum uses this to skip rather than to silently pass: reporting
 * "not detected" for a fault that was never injected is the one result a
 * coverage table must never contain. */
bool plant_fault_applies(plant_board_t board, plant_fault_t f) {
    switch (f) {
    case PF_HIGH_SIDE_SHORT:
    case PF_BUS_SHORT_GND:
    case PF_LOWSIDE_A_SHORT:
    case PF_LOWSIDE_B_SHORT:
    case PF_PACK_COLLAPSE:
        return true; /* every board has a high side, a low side and a pack */

    case PF_BIAS_A_OPEN:
    case PF_BIAS_B_OPEN:
    case PF_BIAS_BUS_OPEN:
    case PF_BLEED_OPEN:
    case PF_TVS_A_SHORT:
    case PF_TVS_B_SHORT:
    case PF_EFUSE_LATCHED:
    case PF_EFUSE_WONT_TURN_OFF:
    case PF_ARM_SWITCH_OPEN:
        return board == PLANT_MK1C; /* bias injection, the eFuse and the
                                     * mechanical disconnect are MK1C     */
    default:
        return false;
    }
}

void plant_set_fault(plant_fault_t f, bool on) {
    plant_t *p = plant_instance();
    if (f < 0 || f >= PF_COUNT)
        return;
    if (!plant_fault_applies(p->board, f))
        return;
    p->faults[f] = on;
}

bool plant_get_fault(plant_fault_t f) {
    plant_t *p = plant_instance();
    return (f >= 0 && f < PF_COUNT) ? p->faults[f] : false;
}

void plant_clear_faults(void) {
    plant_t *p = plant_instance();
    memset(p->faults, 0, sizeof(p->faults));
    p->pack_sag_v = 0.0;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

plant_t *plant_instance(void) {
    if (!g_inited) {
        /* Nothing has chosen a board yet. Take whichever model is in this
         * build -- in a simulator build there is exactly one, so there is
         * no ambiguity to get wrong. The glue calls plant_init() explicitly
         * before the board file's first gpio_put(); this only covers a
         * caller that reads the plant before that. */
        if (plant_have_board(PLANT_MK1C))      plant_init(PLANT_MK1C);
        else if (plant_have_board(PLANT_MK1B)) plant_init(PLANT_MK1B);
        else                                    plant_init(PLANT_MK1A);
    }
    return &g_plant;
}

#define PLANT_N_BOARDS 3
static const plant_ops_t *registry[PLANT_N_BOARDS];

void plant_register(plant_board_t board, const plant_ops_t *ops) {
    if (board >= 0 && board < PLANT_N_BOARDS)
        registry[board] = ops;
}

bool plant_have_board(plant_board_t board) {
    return board >= 0 && board < PLANT_N_BOARDS && registry[board] != NULL;
}

void plant_init(plant_board_t board) {
    const plant_ops_t *ops = plant_have_board(board) ? registry[board] : NULL;
    if (!ops) {
        /* This build does not contain that board's model. Fail loudly:
         * silently substituting another board's network would produce
         * plausible numbers for the wrong hardware, which is the one
         * failure mode a model like this must never have. */
        fprintf(stderr, "plant_init: no model for board %d in this build\n", (int)board);
        abort();
    }
    memset(&g_plant, 0, sizeof(g_plant));
    g_plant.board = board;
    g_plant.ops = ops;
    g_inited = true;
    plant_reset();
}

void plant_reset(void) {
    plant_t *p = &g_plant;
    const plant_ops_t *ops = p->ops;

    memset(p->gpio_level, 0, sizeof(p->gpio_level));
    memset(p->gpio_is_out, 0, sizeof(p->gpio_is_out));
    memset(p->adc_filt, 0, sizeof(p->adc_filt));
    memset(p->adc_primed, 0, sizeof(p->adc_primed));
    memset(p->faults, 0, sizeof(p->faults));

    /* 2S is the DESIGN.md 1.1 variant B pack and the one MK1C's levels are
     * tabulated against. A variant A test calls plant_set_pack_mv(4200). */
    p->pack_mv = 8400.0;
    p->pack_sag_v = 0.0;
    p->bus_pulldown_override = 0.0;
    p->highside_leak_ohms = 0.0;
    p->c_bulk_f = 1.1e-6; /* C115 and the bus's strays, no THT part: the bench decay (DD-054) */

    plant_match_defaults(&p->match[0]);
    plant_match_defaults(&p->match[1]);
    p->i_a = p->i_b = 0.0;
    p->fire_events = 0;
    p->last_fire_channel = 0;

    p->en_hold_v = 0.0;
    p->arm_prev = false;
    p->efuse_on = false;
    p->efuse_latched = false;
    p->bus_ramp_v = 0.0;
    p->t_s = 0.0;

    memset(&p->net, 0, sizeof(p->net));
    net_begin(&p->net, ops->n_nodes);
    if (ops->reset)
        ops->reset(p);

    /* Settle the network so the first ADC read is a real quiescent level
     * and not a zero that happens to look like one. */
    for (int k = 0; k < 200; k++)
        plant_step(0.001);
    p->t_s = 0.0;
}

plant_board_t plant_board(void) { return plant_instance()->board; }
const char *plant_board_name(void) { return plant_instance()->ops->name; }

/* ── GPIO ────────────────────────────────────────────────────────── */

bool plant_gpio(const plant_t *p, int gpio) {
    if (gpio < 0 || gpio >= PLANT_NGPIO)
        return false;
    return p->gpio_level[gpio];
}

void plant_set_gpio(int gpio, bool level) {
    plant_t *p = plant_instance();
    if (gpio < 0 || gpio >= PLANT_NGPIO)
        return;
    p->gpio_level[gpio] = level;
}

void plant_set_gpio_dir(int gpio, bool out) {
    plant_t *p = plant_instance();
    if (gpio < 0 || gpio >= PLANT_NGPIO)
        return;
    p->gpio_is_out[gpio] = out;
}

bool plant_get_gpio(int gpio) {
    plant_t *p = plant_instance();
    if (gpio < 0 || gpio >= PLANT_NGPIO)
        return false;
    bool level;
    if (p->ops->gpio_in && p->ops->gpio_in(p, gpio, &level))
        return level;
    return p->gpio_level[gpio];
}

/* ── ADC ─────────────────────────────────────────────────────────── */

uint16_t plant_adc_counts(int adc_ch) {
    plant_t *p = plant_instance();
    if (adc_ch < 0 || adc_ch > 4)
        return 0;

    /* Channel 4 is the RP2040's internal temperature sensor. Report the
     * count for roughly 27 C so anything that reads it gets a plausible
     * number rather than a floating node. */
    if (adc_ch == 4)
        return 890;

    adc_tap_t t = p->ops->adc_tap(adc_ch);
    if (!t.valid)
        return 0;

    double v = p->adc_filt[adc_ch];

    /* The RP2040 ADC is 12 bit against 3.3 V, and its input clamps. A node
     * above the reference reads full scale -- which is exactly what MK1A's
     * sense pin does during a fire pulse, and the model should say so
     * rather than reporting an impossible count. */
    double counts = v * 4095.0 / 3.3;
    if (counts < 0.0) counts = 0.0;
    if (counts > 4095.0) counts = 4095.0;
    return (uint16_t)(counts + 0.5);
}

/* ── Step ────────────────────────────────────────────────────────── */

void plant_step(double dt_s) {
    plant_t *p = plant_instance();
    if (dt_s <= 0.0)
        return;

    /* Cap the step so a long sleep_ms() in board code does not integrate
     * the charge pump or the match in one lump. Backward Euler stays stable
     * at any step, but the pump's per-edge charge transfer and the match's
     * energy leak are rate processes that want resolving. */
    while (dt_s > 0.0) {
        double max_dt = p->ops->max_dt ? p->ops->max_dt(p) : 200e-6;
        double h = (dt_s > max_dt) ? max_dt : dt_s;
        dt_s -= h;

        net_begin(&p->net, p->ops->n_nodes);
        p->ops->build(p, h);
        net_solve(&p->net, h);
        p->ops->post(p, h);

        match_integrate(p, 0, p->i_a, h);
        match_integrate(p, 1, p->i_b, h);

        /* Tap filters. First order, outside the network -- see adc_tap_t. */
        for (int ch = 0; ch < 4; ch++) {
            adc_tap_t t = p->ops->adc_tap(ch);
            if (!t.valid)
                continue;
            /* node < 0 means the tap is not on a solved node. SNS_VBAT is
             * the case that exists: it hangs on the pack, which the model
             * treats as a source rather than as an unknown. */
            double src = (t.node < 0) ? plant_pack_v(p) : p->net.v[t.node];
            double target = src * t.ratio;
            if (!p->adc_primed[ch]) {
                p->adc_filt[ch] = target;
                p->adc_primed[ch] = true;
            } else if (t.tau_s <= 0.0) {
                p->adc_filt[ch] = target;
            } else {
                double a = h / (t.tau_s + h); /* backward Euler, same as the net */
                p->adc_filt[ch] += (target - p->adc_filt[ch]) * a;
            }
        }

        p->t_s += h;
    }
}

/* ── Test-facing controls and observation ────────────────────────── */

void plant_set_pack_mv(double mv) { plant_instance()->pack_mv = mv; }
void plant_set_bus_pulldown_ohms(double ohms) { plant_instance()->bus_pulldown_override = ohms; }
void plant_set_highside_leak_ohms(double ohms) { plant_instance()->highside_leak_ohms = ohms; }
void plant_set_c_bulk_uf(double uf) { plant_instance()->c_bulk_f = uf * 1e-6; }

plant_match_t *plant_match(int ch) {
    plant_t *p = plant_instance();
    if (ch != 1 && ch != 2)
        return &p->match[0];
    return &p->match[ch - 1];
}

void plant_probe(plant_probe_t *out) {
    plant_t *p = plant_instance();
    memset(out, 0, sizeof(*out));
    out->bus_v = p->net.v[0];
    if (p->ops->n_nodes > 1) out->node_a_v = p->net.v[1];
    if (p->ops->n_nodes > 2) out->node_b_v = p->net.v[2];
    out->vbat_v = plant_pack_v(p);
    out->i_a = p->i_a;
    out->i_b = p->i_b;
    out->en_hold_v = p->en_hold_v;
    out->armed = p->efuse_on;
    out->fired_a = p->match[0].fired;
    out->fired_b = p->match[1].fired;
}

int plant_fire_events(void) { return plant_instance()->fire_events; }
int plant_last_fire_channel(void) { return plant_instance()->last_fire_channel; }
