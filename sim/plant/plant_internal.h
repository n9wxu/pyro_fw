/*
 * Shared guts of the board plants. Only plant.c and plant_mk1*.c include
 * this; everything else uses plant.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PLANT_INTERNAL_H
#define PLANT_INTERNAL_H

#include "plant.h"
#include "net_solve.h"

#define PLANT_NGPIO 30

/* Where an ADC channel taps the network.
 *
 * ratio is the divider between the node and the pin. tau_s is the filter
 * at the tap, which is a lag OUTSIDE the network rather than another node:
 * the divider's output impedance is fixed, so the filter is exactly first
 * order and does not need a node of its own. On MK1C that lag is the 33 us
 * DESIGN.md 4 quotes for (10k || 4.99k) x 10 nF. */
typedef struct {
    bool   valid;
    int    node;
    double ratio;
    double tau_s;
} adc_tap_t;

typedef struct plant_ops {
    const char *name;
    int n_nodes;

    /* Power-on state for whatever the board model keeps outside the
     * network: charge pumps, latches, fuse state. */
    void (*reset)(plant_t *p);

    /* Rebuild the network from the current GPIO levels. Called every step
     * before the solve. */
    void (*build)(plant_t *p, double dt_s);

    /* After the solve: work out the match currents, run any element whose
     * state depends on the result (the eFuse latch, the pack sag), and
     * write i_a / i_b so the shared match integrator can use them. */
    void (*post)(plant_t *p, double dt_s);

    adc_tap_t (*adc_tap)(int adc_ch);

    /* Largest integration step this board can take right now, in seconds.
     * The networks are stiff but backward Euler is stable at any step, so
     * this is about RESOLUTION, not stability: while the charge pump is
     * running, a step longer than half its 100 us period would step over
     * the edges the pump is made of. When nothing is switching, a coarse
     * step keeps a two-minute flight cheap to simulate. */
    double (*max_dt)(plant_t *p);

    /* Pins the board drives back at the MCU: MK1B's two AP2192 FLAG lines.
     * Return false for a pin the board does not source, and the plant
     * reports the last level the firmware itself wrote. */
    bool (*gpio_in)(plant_t *p, int gpio, bool *level);
} plant_ops_t;

struct plant {
    const plant_ops_t *ops;
    plant_board_t board;

    net_t net;

    bool gpio_level[PLANT_NGPIO];
    bool gpio_is_out[PLANT_NGPIO];

    double adc_filt[5];   /* per-channel tap voltage, after the lag        */
    bool   adc_primed[5];

    double pack_mv;
    double bus_pulldown_override; /* 0 = use the design network */
    double highside_leak_ohms;    /* 0 = an ideal open                */
    double pack_sag_v;    /* PF_PACK_COLLAPSE: droop under load            */
    double c_bulk_f;

    bool faults[PF_COUNT];

    plant_match_t match[2];
    double i_a, i_b;      /* written by ops->post, consumed by the shared
                           * match integrator                              */

    int fire_events;
    int last_fire_channel;

    /* MK1C charge pump and eFuse; unused elsewhere but kept here so the
     * probe struct has one home. */
    double en_hold_v;
    bool   arm_prev;
    bool   efuse_on;
    bool   efuse_latched;
    double bus_ramp_v;

    double t_s;           /* model time, for anything rate-based           */
};

/* Each board model registers itself from a constructor.
 *
 * Registration rather than a switch in plant.c, because each plant_mk1*.c
 * includes its own board's board_pins.h and so must be compiled with that
 * board's include path. A simulator build selects one board and links one
 * plant; the plant test binary compiles all three separately and links all
 * three. With a switch, plant.c would need every model in every link.
 *
 * Constructors rather than weak symbols because an undefined weak symbol is
 * not portable: it needs weak_import on Mach-O and weak elsewhere, and this
 * tree builds on the host and under Emscripten. */
void plant_register(plant_board_t board, const plant_ops_t *ops);

#define PLANT_REGISTER(board_enum, ops_ptr)                                                                            \
    __attribute__((constructor)) static void plant_register_##board_enum(void) {                                       \
        plant_register(board_enum, ops_ptr);                                                                           \
    }

/* Helpers the board models share. */
double plant_pack_v(const plant_t *p);
double plant_match_ohms(const plant_match_t *m);  /* including leak_ohm */
bool   plant_gpio(const plant_t *p, int gpio);

/* A logic-level FET's channel resistance, chosen by its gate level and by
 * whether a drain-source short has been injected. AO3400A on-resistance at
 * a 3.3 V gate is about 30 mohm. */
double plant_fet_ohms(const plant_t *p, bool gate_high, plant_fault_t short_fault);

#endif
