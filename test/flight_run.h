/*
 * A flight flown beside its truth [TST-03]: the firmware through the board
 * harness, the rocket through sim/mach_plant.c, and what each did.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_RUN_H
#define FLIGHT_RUN_H

#include "mach_plant.h"
#include "../src/estimator.h"
#include "../src/pyro_limits.h"
#include <stdbool.h>
#include <stdint.h>

/* The pads: 10 C at sea level, 45 C at 2000 m, and the standard atmosphere's
 * own, where a pressure altitude is the true height. */
extern const mp_site_t COLD, HOT, ISA;

typedef struct {
    const char *name;
    mp_rocket_t r;
} rocket_t;

/* Tuned so each does what its name says at the cold pad. */
enum { HOP, SUBSONIC, MID_MACH, DRAGGY, LOW_DRAG, BOOST_30G, TO_20_KM, TO_30_KM, TO_45_KM, N_ROCKETS };
extern const rocket_t ROCKETS[N_ROCKETS];

/* All zero is a clean flight on the default configuration, in metres: pyro 1
 * opens the rocket's canopy on its first pulse, pyro 2 deploys nothing. */
typedef struct {
    const char *config;    /* config.ini lines after "[pyro]" and "units=m"; NULL: none */
    const char *estimator; /* the one obeyed; NULL: the default */

    /* What each channel's charge does. rate_ms is the canopy's descent rate
     * in the pad's air; 0 on pyro 1 takes the rocket's, 0 on pyro 2 deploys
     * nothing. lights_on_pulse is which of the channel's pulses lights the
     * charge: 0 or 1 the first, n the nth, -1 never. */
    float rate_ms[2];
    int lights_on_pulse[2];
    bool thin_air;             /* a canopy's rate goes as 1/sqrt(density) */
    float canopy_lost_below_m; /* whatever is out is lost on passing this height; 0: never */

    /* The board. A faulted channel reads open to the health check; a board
     * that energises nothing takes each command and its protection ends it. */
    bool pyro_faulted[2];
    bool energises_nothing;
    const pyro_limits_t *limits; /* NULL: the general ones */

    mp_port_t port;     /* all zero: clean ports */
    mp_charge_t charge; /* what a charge that lights adds to the bay's pressure */

    float dropout_at_s, dropout_s; /* no readings */
    float stuck_at_s, stuck_s;     /* stuck_s 0: for good */
    float glitch_at_s;             /* glitch_pa added to glitch_n readings from here */
    int32_t glitch_pa;
    int glitch_n;
    float sensor_rms_pa;    /* 0: the harness's */
    float swing_rms_pa;     /* under a canopy, until touchdown */
    float coast_noise_pa;   /* from burnout... */
    float coast_noise_to_s; /* ...until this flight time; 0: until the true apogee */
    uint32_t interval_ms;   /* between readings; 0: the test HAL's 20 ms */

    uint16_t main_m;        /* shorthand: an AGL trigger on pyro 2 at this height */
    float main_ms;          /* shorthand for rate_ms[1] */
    bool to_landed;         /* on past touchdown to LANDED, with the landing timeout off */
    bool stop_after_apogee; /* 2 s after the true apogee */
    float pad_s;            /* on the pad before ignition; 0: 10 s */

    /* A restart in flight [FLT-BROWN-02]: the processor starts again with
     * nothing but its stored files. restart_cause is a reset_cause_t. */
    float restart_at_s; /* 0: none */
    int restart_cause;

    uint32_t (*loop_lag)(uint32_t t); /* the loop clock's lateness; NULL: none */
    /* Network, script and storage activity holds the flight software for 40
     * to 73 ms at a time, about 1.3 times a second [FLT-RT-01]. */
    bool stalls;
} flight_conditions_t;

#define FLOWN_PULSES_MAX 64

typedef struct {
    float t, h, v; /* flight time, and the truth then; v is positive up */
    uint8_t channel;
    bool energised;
} flown_pulse_t;

typedef struct {
    bool launched;
    float launch_t, launch_h; /* when the firmware declared it, and the true height then */

    float apogee_t, apogee_h, max_mach; /* the truth */
    bool apogee_declared;
    float declared_t; /* when the firmware declared apogee */
    bool armed;
    float armed_t, armed_h; /* when, and the true height then */
    float thrust_end_t;     /* when the thrust report ended; 0: it never began */

    flown_pulse_t pulse[FLOWN_PULSES_MAX];
    int pulses;
    /* The first pulse of each channel. drogue and main are pyro 1 and pyro 2. */
    bool drogue, main;
    float drogue_t, main_t, main_h;

    /* What each estimator's own apogee detector said, obeyed or not, in
     * estimator_at()'s order. */
    bool said_apogee[ESTIMATORS_MAX];
    float said_t[ESTIMATORS_MAX];

    int32_t peak_cm;
    bool peak_lower_bound;
    float touchdown_t, landed_t; /* 0: not reached */
    int final_state;             /* flight_state_t when the run stopped */
    int32_t final_altitude_cm;
    float fastest_descent_ms; /* the truth, in the pad's air */
} flown_t;

flown_t fly(const mp_rocket_t *rocket, const mp_site_t *site, const flight_conditions_t *conditions, uint32_t seed,
            float until_s);

/* The plant alone, to its apogee. */
float plant_apogee_m(const mp_rocket_t *rocket, const mp_site_t *site);
float plant_apogee_s(const mp_rocket_t *rocket, const mp_site_t *site);

#endif
