/*
 * Board plant models — the electrical half of the simulator.
 *
 * sim/physics.c models the rocket. This models the BOARD: the firing bus,
 * the sense dividers, the bias injectors, the switches and the e-match. It
 * exists so the real board code under boards/<name>/pyro_board.c can run on
 * the host against something that answers like hardware, instead of against
 * the fixture in boards/sim/hal_sim.c where continuity is whatever the test
 * last wrote.
 *
 * The plant is driven by sim/hw/, a shim for the small part of the Pico SDK
 * that the board code touches. gpio_put() lands here as plant_set_gpio();
 * adc_read() lands here as plant_adc_counts(). Nothing in boards/ changes.
 *
 *   sim/physics.c ──pressure──► flight_states.c
 *                                     │ pyro_fire()
 *                                     ▼
 *                        boards/mk1c/pyro_board.c   (the real file)
 *                                     │ gpio_put / adc_read
 *                                     ▼
 *                        sim/hw/rp2040_shim.c
 *                                     │
 *                                     ▼
 *                        sim/plant/plant_mk1c.c ──fired──► physics deploy
 *
 * Every board's model is a resistive network with node capacitance, solved
 * by backward Euler in net_solve.c. That choice is what makes the RC settle
 * times real rather than asserted: MK1A's 10.1 ms open-channel rise and
 * MK1C's bias-release decay both fall out of the network instead of being
 * hardcoded, so firmware that samples too early reads the wrong number here
 * exactly as it would on the bench.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PLANT_H
#define PLANT_H

#include <stdint.h>
#include <stdbool.h>

/* ── The e-match ──────────────────────────────────────────────────
 *
 * Properties and their limits come from the approval gate in
 * ~/Documents/pyro_mk1c/DESIGN.md 1.2, rows M1-M13. The row is named on
 * each field so a value here can be checked against the table it came
 * from. Defaults are the TYPICAL column; a margin test should set the
 * worst-case end itself, because DESIGN.md is explicit that a typical
 * value must never carry a margin calculation. */
typedef enum {
    MATCH_ABSENT = 0,  /* nothing in the connector                        */
    MATCH_PRESENT,     /* a bridgewire of r_ohm                           */
    MATCH_SHORT,       /* dead short across the terminals                 */
    MATCH_SPENT_OPEN,  /* M7 typical: ignition consumed the bridgewire    */
    MATCH_SPENT_SHORT, /* M13 worst case: a spent match can be a short    */
} match_state_t;

typedef struct {
    match_state_t state;
    double r_ohm;       /* M1  bridgewire, new: 0.8 / 1.0 / 1.6           */
    double leak_ohm;    /* a dirty connector in parallel; 0 = none.
                         * DESIGN.md calls out 5 kohm as the reading that
                         * passes a naive threshold.                      */
    double no_fire_a;   /* M2  no-fire current, 100 mA minimum            */
    double all_fire_a;  /* M4  all-fire current, 1 A maximum              */
    double all_fire_hold_s; /* the time qualifier on M4. An all-fire
                         * current is a current held for a duration; the
                         * datasheet value without its qualifier is not a
                         * criterion. Default 50 ms.                      */
    double fire_energy_j; /* M5 ignition energy, 15 mJ maximum            */
    double thermal_tau_s; /* M6 bridgewire thermal constant, 3 ms minimum */

    /* What it becomes once it fires. M7 says open is typical and M13 says
     * a short is the worst case; the S8 approach doctrine in DESIGN.md
     * treats both as "still present", so a test that cares must be able to
     * choose. */
    match_state_t spent_as;

    /* ── integrator state ───────────────────────────────────────── */
    /* Ignition has two independent criteria, because M4 and M5 describe
     * two different regimes and neither implies the other:
     *
     *   energy   -- a fast pulse. Energy accumulates in the bridgewire and
     *               leaks at thermal_tau_s, so a pulse shorter than tau is
     *               adiabatic and a slow one is not. This is the criterion
     *               DESIGN.md 7.3 argues the capacitor discharge satisfies.
     *   sustained -- a current at or above all_fire_a held for
     *               all_fire_hold_s. This is how variant A fires: 3.5 A
     *               current-limited from U9, well under the energy a
     *               capacitor would dump but far over the all-fire current.
     *
     * A model with only the energy criterion never fires variant A, and one
     * with only the current criterion never fires on a 1 ms pulse. */
    double energy_j;  /* accumulated in the bridgewire, leaked at tau     */
    double all_fire_run_s; /* time so far at or above all_fire_a          */
    double last_i_a;  /* last solved current, for reporting              */
    bool   fired;     /* latched: energy crossed fire_energy_j           */
    double no_fire_exposure_s; /* M3: cumulative time above no_fire_a.
                                * A test can assert the board never sat a
                                * match above its no-fire current for longer
                                * than the datasheet's qualifier.        */
} plant_match_t;

/* Fill with the M-row typicals: 1.0 ohm, 100 mA no-fire, 1 A all-fire,
 * 15 mJ, 3 ms, spent open. */
void plant_match_defaults(plant_match_t *m);

/* ── Injected faults ─────────────────────────────────────────────
 *
 * Named for the DESIGN.md 8.2 row or the 8.1 symptom each one produces.
 * Not every fault applies to every board; plant_set_fault() ignores one
 * the selected board has no element for, and plant_fault_applies() says
 * whether it does, so a sweep over the whole enum can skip cleanly rather
 * than silently reporting a pass for a fault it never injected. */
typedef enum {
    PF_HIGH_SIDE_SHORT = 0, /* 8.1: bus at pack voltage, pump stopped     */
    PF_BUS_SHORT_GND,       /* 8.1: bus will not rise under its own bias  */
    PF_LOWSIDE_A_SHORT,     /* Q103 drain-source short (channel A)        */
    PF_LOWSIDE_B_SHORT,     /* Q104 drain-source short (channel B)        */
    PF_BIAS_A_OPEN,         /* R112/D104 open: a live channel reads open  */
    PF_BIAS_B_OPEN,         /* R117/D106 open                             */
    PF_BIAS_BUS_OPEN,       /* R120/D107 open                             */
    PF_BLEED_OPEN,          /* R_BLEED open: the only discharge path gone */
    PF_TVS_A_SHORT,         /* shorted TVS reads like a present match     */
    PF_TVS_B_SHORT,
    PF_EFUSE_LATCHED,       /* U9 latched off; it will not arm            */
    PF_EFUSE_WONT_TURN_OFF, /* U9 stuck on: the pump stops, the bus does not */
    PF_ARM_SWITCH_OPEN,     /* the mechanical disconnect is out           */
    PF_PACK_COLLAPSE,       /* the pack sags under the pulse              */
    PF_COUNT
} plant_fault_t;

const char *plant_fault_name(plant_fault_t f);

/* ── The plant ───────────────────────────────────────────────────── */

typedef enum { PLANT_MK1A = 0, PLANT_MK1B, PLANT_MK1C } plant_board_t;

typedef struct plant plant_t;

/* One plant per process. The firmware is a singleton (its board state is
 * file-static), so a second instance could not be wired to it anyway. */
plant_t *plant_instance(void);
void     plant_init(plant_board_t board);

/* Whether this build contains a model for that board. A simulator build
 * links one; the plant test binary links all three. */
bool     plant_have_board(plant_board_t board);
void     plant_reset(void);
plant_board_t plant_board(void);
const char   *plant_board_name(void);

/* ── Wiring to the SDK shim ──────────────────────────────────────── */

void     plant_set_gpio(int gpio, bool level);  /* gpio_put()          */
bool     plant_get_gpio(int gpio);              /* gpio_get()          */
void     plant_set_gpio_dir(int gpio, bool out);/* gpio_set_dir()      */
uint16_t plant_adc_counts(int adc_ch);          /* adc_read(), 0-4095  */

/* Advance the network by dt seconds. The shim calls this from every
 * function that consumes time -- sleep_ms(), busy_wait_us(), and the
 * simulated main-loop tick -- so a blocking settle in board code settles
 * the model too. */
void plant_step(double dt_s);

/* ── Test-facing controls ────────────────────────────────────────── */

void plant_set_pack_mv(double mv);      /* 1S 4200, 2S 8400 (DESIGN.md 1.1) */

/* Override the bus pull-down with a measured value, in ohms; 0 restores the
 * one the schematic implies. For asking what the firmware does when the
 * as-built board does not match the design -- see the bench decay-constant
 * note at the top of plant_mk1c.c. */
void plant_set_bus_pulldown_ohms(double ohms);

/* Off-state leakage of a high-side switch, in ohms; 0 (the default) means
 * an ideal open.
 *
 * Default ideal because the levels the board files tabulate were computed
 * that way, and a model that quietly added leakage would disagree with
 * them for a reason no one could see. It matters on MK1A and MK1B, where
 * the pull-up is 100 kohm: a DMC2053 leaking a microamp at 8.4 V is about
 * 8 Mohm, which lifts a 10 kohm-leakage reading by 9 counts. Set it when
 * asking what a leaking high side would do. */
void plant_set_highside_leak_ohms(double ohms);
void plant_set_c_bulk_uf(double uf);    /* variant A 100, variant B 470-2200 */
void plant_set_fault(plant_fault_t f, bool on);
bool plant_get_fault(plant_fault_t f);
bool plant_fault_applies(plant_board_t board, plant_fault_t f);
void plant_clear_faults(void);

plant_match_t *plant_match(int ch);     /* ch is 1 or 2 */

/* ── Observation ─────────────────────────────────────────────────── */

typedef struct {
    double bus_v;      /* firing bus (MK1C) / initiator common (MK1A)     */
    double node_a_v;   /* channel A sense node, before the divider        */
    double node_b_v;
    double vbat_v;
    double i_a;        /* current through match A, amps                   */
    double i_b;
    double en_hold_v;  /* MK1C: C_HOLD, the charge pump's output          */
    bool   armed;      /* MK1C: U9 conducting                             */
    bool   fired_a;    /* the match has taken its ignition energy         */
    bool   fired_b;
} plant_probe_t;

void plant_probe(plant_probe_t *out);

/* Rising-edge count of each match's fired latch, so a closed-loop driver
 * can deploy a chute in sim/physics.c the moment the plant says the match
 * actually took its energy -- rather than when the firmware merely
 * commanded a fire, which is the distinction this whole model exists to
 * make. */
int plant_fire_events(void);
int plant_last_fire_channel(void);

#endif
