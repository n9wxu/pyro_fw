/*
 * The board plants: each board's firing and sense network, for its real
 * pyro_board.c to run against through the SDK stand-in in sim/hw/. A
 * resistive network with node capacitance, solved by backward Euler
 * (net_solve.h), so settle times come out of the RC rather than a table.
 * See sim/plant/README.md "What is modelled" and "What is not modelled".
 * "DESIGN.md" is the MK1C design record (pyro_mk1c/DESIGN.md, on the author's
 * machine and not in this repository); its section and row numbers are cited as they
 * stood when the model was written.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PLANT_H
#define PLANT_H

#include <stdint.h>
#include <stdbool.h>

/* ── The e-match ──────────────────────────────────────────────────
 *
 * Each field names its row of DESIGN.md 1.2's e-match approval gate,
 * M1-M13. The defaults are the typical column; a margin test sets the
 * worst case itself. */
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
    double leak_ohm;    /* a dirty connector in parallel; 0 = none       */
    double no_fire_a;   /* M2  no-fire current, 100 mA minimum            */
    double all_fire_a;  /* M4  all-fire current, 1 A maximum              */
    double all_fire_hold_s; /* M4's time qualifier; default 50 ms        */
    double fire_energy_j; /* M5 ignition energy, 15 mJ maximum            */
    double thermal_tau_s; /* M6 bridgewire thermal constant, 3 ms minimum */

    /* What it becomes once fired: M7's typical open or M13's worst-case
     * short. */
    match_state_t spent_as;

    /* ── integrator state ───────────────────────────────────────── */
    /* Ignition on either of two criteria, since M4 and M5 are different
     * regimes: energy (M5, leaking at thermal_tau_s) fires a fast pulse,
     * and all_fire_a held for all_fire_hold_s (M4) fires a slow drive. */
    double energy_j;  /* accumulated in the bridgewire, leaked at tau     */
    double all_fire_run_s; /* time so far at or above all_fire_a          */
    double last_i_a;  /* last solved current, for reporting              */
    bool   fired;     /* latched: energy crossed fire_energy_j           */
    double no_fire_exposure_s; /* M3: cumulative time above no_fire_a      */
} plant_match_t;

/* Fill with the M-row typicals: 1.0 ohm, 100 mA no-fire, 1 A all-fire,
 * 15 mJ, 3 ms, spent open. */
void plant_match_defaults(plant_match_t *m);

/* ── Injected faults ─────────────────────────────────────────────
 *
 * Named for the DESIGN.md 8.2 row or 8.1 symptom each produces.
 * plant_set_fault() ignores one the board has no element for;
 * plant_fault_applies() says which. */
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

/* One plant per process, as the firmware's board state is file-static. */
plant_t *plant_instance(void);
void     plant_init(plant_board_t board);

/* A simulator build links one model; the plant tests link all three. */
bool     plant_have_board(plant_board_t board);
void     plant_reset(void);
plant_board_t plant_board(void);
const char   *plant_board_name(void);

/* ── Wiring to the SDK shim ──────────────────────────────────────── */

void     plant_set_gpio(int gpio, bool level);  /* gpio_put()          */
bool     plant_get_gpio(int gpio);              /* gpio_get()          */
void     plant_set_gpio_dir(int gpio, bool out);/* gpio_set_dir()      */
void     plant_set_gpio_pull_up(int gpio, bool up); /* else pulled down  */
uint16_t plant_adc_counts(int adc_ch);          /* adc_read(), 0-4095  */

/* Advance by dt seconds. The shim calls it from everything that takes time. */
void plant_step(double dt_s);

/* ── Test-facing controls ────────────────────────────────────────── */

void plant_set_pack_mv(double mv);      /* 1S 4200, 2S 8400 (DESIGN.md 1.1) */

/* MK1C's bus pull-down, in ohms, for a board that does not match the design;
 * 0 restores the schematic's. */
void plant_set_bus_pulldown_ohms(double ohms);

/* Off-state leakage of a high-side switch, in ohms; 0 (the default) is an
 * ideal open, as the board files' tabulated levels assume. Against MK1A's
 * and MK1B's 100 kohm pull-ups a DMC2053 leaking 1 uA at 8.4 V (8 Mohm)
 * lifts a 10 kohm reading by 9 counts. */
void plant_set_highside_leak_ohms(double ohms);
/* MK1B's U5 as fitted (the default) or as the netlist draws it. The
 * AP2192A discharges its disabled outputs, which holds both sense nodes
 * near 0 V (DD-059). */
typedef enum { MK1B_U5_AP2192A = 0, MK1B_U5_AP2192 } plant_mk1b_u5_t;
void plant_set_mk1b_u5(plant_mk1b_u5_t part);

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

/* Fires the plant counted: a match that took its energy, not a fire the
 * firmware commanded. */
int plant_fire_events(void);
int plant_last_fire_channel(void);

#endif
