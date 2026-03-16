/*
 * Rocket physics engine — shared between WASM, CLI sim, and tests.
 * SPDX-License-Identifier: MIT
 */
#include "physics.h"
#include <math.h>

#define G 9.81f
#define DT 0.001f
#define GROUND_PA 101325.0f

/* ── Atmosphere model ─────────────────────────────────────────────── */

float physics_pressure_pa(float alt_m) {
    if (alt_m < 11000.0f) {
        return GROUND_PA * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
    }
    /* Stratosphere (11-47km): isothermal at 216.65K */
    float p11 = GROUND_PA * powf(1.0f - 0.0065f * 11000.0f / 288.15f, 5.2561f);
    if (alt_m < 47000.0f) {
        return p11 * expf(-9.81f * (alt_m - 11000.0f) / (287.05f * 216.65f));
    }
    /* Above 47km */
    float p47 = p11 * expf(-9.81f * 36000.0f / (287.05f * 216.65f));
    return p47 * expf(-9.81f * (alt_m - 47000.0f) / (287.05f * 270.65f));
}

/* ── Burn time solver ─────────────────────────────────────────────── */

static float compute_burn_time(float thrust_accel, float target_m) {
    float lo = 0.0f, hi = 200.0f;
    for (int i = 0; i < 50; i++) {
        float t = (lo + hi) / 2.0f;
        float a_net = thrust_accel - G;
        float v_bo = a_net * t;
        float h_burn = 0.5f * a_net * t * t;
        float h_coast = v_bo * v_bo / (2.0f * G);
        if (h_burn + h_coast < target_m)
            lo = t;
        else
            hi = t;
    }
    return (lo + hi) / 2.0f;
}

/* ── Struct-based API ─────────────────────────────────────────────── */

void physics_reset(physics_state_t *ps) {
    ps->alt_m = 0;
    ps->vel_ms = 0;
    ps->thrust_accel = 0;
    ps->burn_time = 0;
    ps->apogee_m = 0;
    ps->drogue_drag = 0.8f;
    ps->main_drag = 4.0f;
    ps->ballistic_drag = 0.05f;
    ps->drogue_deployed = false;
    ps->main_deployed = false;
    ps->on_ground = false;
}

void physics_init(physics_state_t *ps, float target_alt_m) {
    physics_reset(ps);
    if (target_alt_m > 50000.0f)
        ps->thrust_accel = 5.0f * G;
    else if (target_alt_m > 500.0f)
        ps->thrust_accel = 10.0f * G;
    else
        ps->thrust_accel = 20.0f * G;
    ps->burn_time = compute_burn_time(ps->thrust_accel, target_alt_m);
}

void physics_set_profile(physics_state_t *ps, float thrust_accel, float burn_time_s) {
    ps->thrust_accel = thrust_accel;
    ps->burn_time = burn_time_s;
}

void physics_set_drag(physics_state_t *ps, float drogue, float main_chute, float ballistic) {
    ps->drogue_drag = drogue;
    ps->main_drag = main_chute;
    ps->ballistic_drag = ballistic;
}

void physics_step(physics_state_t *ps, float flight_t_s) {
    if (ps->on_ground)
        return;

    float a = -G;
    if (flight_t_s < ps->burn_time)
        a += ps->thrust_accel;

    if (ps->vel_ms < 0.0f) {
        float drag = ps->ballistic_drag;
        if (ps->main_deployed)
            drag = ps->main_drag;
        else if (ps->drogue_deployed)
            drag = ps->drogue_drag;
        /* Scale drag by atmospheric density (exponential decay) */
        float density_frac = expf(-ps->alt_m / 8500.0f);
        a += drag * density_frac * (-ps->vel_ms);
    }

    ps->vel_ms += a * DT;
    ps->alt_m += ps->vel_ms * DT;

    if (ps->alt_m > ps->apogee_m)
        ps->apogee_m = ps->alt_m;

    if (ps->alt_m <= 0.0f) {
        ps->alt_m = 0;
        ps->vel_ms = 0;
        ps->on_ground = true;
    }
}

void physics_deploy_drogue(physics_state_t *ps) {
    ps->drogue_deployed = true;
}

void physics_deploy_main(physics_state_t *ps) {
    ps->main_deployed = true;
}

/* ── WASM flat API (global singleton) ─────────────────────────────── */

static physics_state_t g_physics;

void physics_wasm_init(float target_alt_m) {
    physics_init(&g_physics, target_alt_m);
}

void physics_wasm_reset(void) {
    physics_reset(&g_physics);
}

void physics_wasm_step(float flight_t_s) {
    physics_step(&g_physics, flight_t_s);
}

void physics_wasm_deploy_drogue(void) {
    physics_deploy_drogue(&g_physics);
}

void physics_wasm_deploy_main(void) {
    physics_deploy_main(&g_physics);
}

float physics_wasm_alt_m(void) {
    return g_physics.alt_m;
}
float physics_wasm_vel_ms(void) {
    return g_physics.vel_ms;
}
float physics_wasm_pressure_pa(void) {
    return physics_pressure_pa(g_physics.alt_m);
}
float physics_wasm_apogee_m(void) {
    return g_physics.apogee_m;
}
int physics_wasm_on_ground(void) {
    return g_physics.on_ground ? 1 : 0;
}
int physics_wasm_drogue_deployed(void) {
    return g_physics.drogue_deployed ? 1 : 0;
}
int physics_wasm_main_deployed(void) {
    return g_physics.main_deployed ? 1 : 0;
}
