/*
 * A flight the bench can fly: a profile of altitude and the pressure the
 * International Standard Atmosphere puts there [DD-078].
 *
 * While one runs, the pressure sensor's readings are replaced by the
 * profile's, so the whole flight software -- launch, the Mach lockout,
 * apogee, deployment, landing -- and both logs fly it on the real board,
 * with the real IMU logging a stationary board beside it.
 *
 *   pad    held at the ground for pad_s
 *   boost  constant acceleration for boost_s, sized so the coast that
 *          follows, under gravity alone, peaks at apogee_m
 *   coast  ballistic to apogee
 *   drogue descent at drogue_ms, scaled by sqrt(rho0 / rho(h)) when
 *          thin_air is set: a parachute falls faster in thin air
 *   main   from main_alt_m, at main_ms
 *
 * Altitudes are above the pad. Pressure is the ISA's at the pad's own
 * altitude plus the profile's, the pad's taken from the pressure it reads,
 * to 32 km (1976 US Standard Atmosphere, the three lowest layers).
 *
 * Pure: host-tested (test_flight_sim.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_SIM_H
#define FLIGHT_SIM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float apogee_m;
    float boost_s;
    float drogue_ms;
    float main_alt_m;
    float main_ms;
    bool thin_air;
    float pad_s;
} fsim_params_t;

typedef enum { FSIM_PAD, FSIM_BOOST, FSIM_COAST, FSIM_DROGUE, FSIM_MAIN, FSIM_LANDED } fsim_phase_t;

typedef struct {
    fsim_params_t p;
    float accel; /* boost, m/s^2 */
    float h_burn, v_burn, t_burn, t_apogee;
    float pad_msl; /* the pad's ISA altitude, from its pressure */
    /* descent, stepped forward as time is asked for */
    float t_desc, h_desc;
    fsim_phase_t phase;
    float t_landed;
} fsim_t;

/* The 1976 US Standard Atmosphere below 32 km: pressure at a geopotential
 * altitude, and back. */
float fsim_isa_pressure(float h_msl);
float fsim_isa_altitude(float pa);
float fsim_isa_density(float h_msl);

/* False for a profile that cannot be flown (no boost, apogee not above the
 * main deployment, a rate of zero). ground_pa: what the pad reads. */
bool fsim_start(fsim_t *s, const fsim_params_t *p, float ground_pa);

/* Altitude above the pad t seconds after start. Descent is integrated
 * forward, so t must not go backwards once the descent has begun. */
float fsim_altitude(fsim_t *s, float t);

/* The pressure there. */
float fsim_pressure(fsim_t *s, float t);

fsim_phase_t fsim_phase(const fsim_t *s);
const char *fsim_phase_name(fsim_phase_t ph);

#endif
