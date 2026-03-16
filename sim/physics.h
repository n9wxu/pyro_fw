/*
 * Rocket physics engine for simulation.
 *
 * Standard atmosphere model (troposphere + stratosphere),
 * configurable thrust profiles, and drag with density falloff.
 * Used by WASM sim, CLI sim, and closed-loop tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Atmosphere ───────────────────────────────────────────────────── */

/** Standard atmosphere pressure (Pa) for a given altitude (m).
 *  Accurate through troposphere (0-11km), stratosphere (11-47km),
 *  and mesosphere approximation above 47km. */
float physics_pressure_pa(float alt_m);

/* ── Physics state ────────────────────────────────────────────────── */

typedef struct {
    float alt_m;          /* altitude in meters above ground */
    float vel_ms;         /* velocity in m/s (positive = up) */
    float thrust_accel;   /* thrust acceleration in m/s² */
    float burn_time;      /* motor burn time in seconds */
    float apogee_m;       /* peak altitude reached */
    float drogue_drag;    /* drogue drag coefficient (default 0.8) */
    float main_drag;      /* main chute drag coefficient (default 4.0) */
    float ballistic_drag; /* no-chute drag coefficient (default 0.05) */
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

/** Reset physics state and configure for a target apogee altitude.
 *  Automatically computes thrust and burn time. */
void physics_init(physics_state_t *ps, float target_alt_m);

/** Reset physics state to all zeros */
void physics_reset(physics_state_t *ps);

/** Set a custom flight profile (thrust acceleration + burn time) */
void physics_set_profile(physics_state_t *ps, float thrust_accel, float burn_time_s);

/** Set custom drag coefficients */
void physics_set_drag(physics_state_t *ps, float drogue, float main_chute, float ballistic);

/** Step physics by 1ms. flight_t is seconds since launch. */
void physics_step(physics_state_t *ps, float flight_t_s);

/** Deploy drogue chute */
void physics_deploy_drogue(physics_state_t *ps);

/** Deploy main chute */
void physics_deploy_main(physics_state_t *ps);

/* ── WASM-friendly flat API (no structs across boundary) ──────────── */

/** Initialize physics for a target altitude (meters) */
void physics_wasm_init(float target_alt_m);

/** Reset physics */
void physics_wasm_reset(void);

/** Step by 1ms. flight_t_s = seconds since launch */
void physics_wasm_step(float flight_t_s);

/** Deploy drogue */
void physics_wasm_deploy_drogue(void);

/** Deploy main chute */
void physics_wasm_deploy_main(void);

/** Get current altitude (m) */
float physics_wasm_alt_m(void);

/** Get current velocity (m/s, positive=up) */
float physics_wasm_vel_ms(void);

/** Get current pressure (Pa) */
float physics_wasm_pressure_pa(void);

/** Get peak altitude (m) */
float physics_wasm_apogee_m(void);

/** Is rocket on the ground? */
int physics_wasm_on_ground(void);

/** Is drogue deployed? */
int physics_wasm_drogue_deployed(void);

/** Is main deployed? */
int physics_wasm_main_deployed(void);

#ifdef __cplusplus
}
#endif

#endif /* PHYSICS_H */
