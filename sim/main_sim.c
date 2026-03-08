/*
 * Pyro MK1B Simulation — closed-loop flight with physics.
 *
 * Runs the real flight software against a physics model.
 * Pyro fires feed back into the physics (chute deployment).
 * Can be compiled for host (CLI) or WASM (browser).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "../src/hal.h"
#include "../src/flight_states.h"
#include "../src/buzzer.h"
#include "hal_sim.h"

/* ── Physics ──────────────────────────────────────────────────────── */

#define G           9.81f
#define DT          0.001f
#define GROUND_PA   101325.0f
#define PAD_DWELL_MS 2000

#define DROGUE_DRAG  0.8f
#define MAIN_DRAG    4.0f

typedef struct {
    float alt_m;
    float vel_ms;
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

typedef struct {
    float target_alt_m;
    float thrust_accel;
    float burn_time;
} flight_profile_t;

/* Standard atmosphere pressure model */
static float alt_m_to_pa(float alt_m) {
    if (alt_m < 11000.0f)
        return GROUND_PA * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
    float p11 = GROUND_PA * powf(1.0f - 0.0065f * 11000.0f / 288.15f, 5.2561f);
    if (alt_m < 47000.0f)
        return p11 * expf(-9.81f * (alt_m - 11000.0f) / (287.05f * 216.65f));
    float p47 = p11 * expf(-9.81f * 36000.0f / (287.05f * 216.65f));
    return p47 * expf(-9.81f * (alt_m - 47000.0f) / (287.05f * 270.65f));
}

static flight_profile_t make_profile(float target_m) {
    flight_profile_t p = { .target_alt_m = target_m };
    if (target_m > 50000.0f)      p.thrust_accel = 5.0f * G;
    else if (target_m > 500.0f)   p.thrust_accel = 10.0f * G;
    else                          p.thrust_accel = 20.0f * G;
    float lo = 0.0f, hi = 200.0f;
    for (int i = 0; i < 50; i++) {
        float t = (lo + hi) / 2.0f;
        float a_net = p.thrust_accel - G;
        float v_bo = a_net * t;
        float h = 0.5f * a_net * t * t + v_bo * v_bo / (2.0f * G);
        if (h < target_m) lo = t; else hi = t;
    }
    p.burn_time = (lo + hi) / 2.0f;
    return p;
}

static void physics_step(physics_state_t *ps, float flight_t, const flight_profile_t *p) {
    if (ps->on_ground) return;
    float a = -G;
    if (flight_t < p->burn_time) a += p->thrust_accel;
    if (ps->vel_ms < 0.0f) {
        float drag = 0.05f;
        if (ps->main_deployed) drag = MAIN_DRAG;
        else if (ps->drogue_deployed) drag = DROGUE_DRAG;
        float density = expf(-ps->alt_m / 8500.0f);
        a += drag * density * (-ps->vel_ms);
    }
    ps->vel_ms += a * DT;
    ps->alt_m += ps->vel_ms * DT;
    if (ps->alt_m <= 0.0f) { ps->alt_m = 0; ps->vel_ms = 0; ps->on_ground = true; }
}

/* ── Simulation runner ────────────────────────────────────────────── */

typedef struct {
    flight_context_t ctx;
    physics_state_t phys;
    flight_profile_t profile;
    float apogee_m;
    uint32_t step_ms;
    uint32_t max_ms;
    int prev_fires;
} sim_state_t;

static sim_state_t sim;

void sim_init_flight(float target_alt_m, const char *config_ini) {
    sim_reset();
    memset(&sim, 0, sizeof(sim));
    sim.profile = make_profile(target_alt_m);
    sim.step_ms = (target_alt_m > 50000.0f) ? 50 : 1;
    sim.max_ms = (target_alt_m > 50000.0f) ? 15000000
               : (target_alt_m > 500.0f)   ? 600000
               :                              120000;

    /* Write config if provided */
    if (config_ini && config_ini[0])
        hal_fs_write_file("config.ini", config_ini, strlen(config_ini));

    flight_init(&sim.ctx);
}

/* Returns 1 if flight complete (LANDED), 0 if still running */
int sim_tick(void) {
    uint32_t t = hal_time_ms() + sim.step_ms;
    sim_set_time(t);

    if (t > sim.max_ms) return 1;

    /* Closed-loop: check pyro fires, update physics */
    int fires = sim_get_pyro_fire_count();
    if (fires > sim.prev_fires) {
        uint8_t ch = sim_get_pyro_last_channel();
        if (ch == 1 && !sim.phys.drogue_deployed) sim.phys.drogue_deployed = true;
        if (ch == 2 && !sim.phys.main_deployed)   sim.phys.main_deployed = true;
        sim.prev_fires = fires;
    }
    sim_clear_pyro_firing();

    /* Physics */
    if (t >= PAD_DWELL_MS) {
        float flight_t = (float)(t - PAD_DWELL_MS) / 1000.0f;
        for (uint32_t s = 0; s < sim.step_ms; s++)
            physics_step(&sim.phys, flight_t + (float)s * DT, &sim.profile);
    }
    if (sim.phys.alt_m > sim.apogee_m) sim.apogee_m = sim.phys.alt_m;

    /* Feed firmware */
    sim_set_pressure(alt_m_to_pa(sim.phys.alt_m));
    sim.ctx.current_state = dispatch_state(&sim.ctx, t);
    flight_update_outputs(&sim.ctx, t);

    return (sim.ctx.current_state == LANDED) ? 1 : 0;
}

/* Accessors for the simulation state */
const flight_context_t *sim_get_ctx(void)  { return &sim.ctx; }
float sim_get_altitude_m(void)             { return sim.phys.alt_m; }
float sim_get_velocity_ms(void)            { return sim.phys.vel_ms; }
float sim_get_apogee_m(void)               { return sim.apogee_m; }
bool  sim_get_drogue_deployed(void)        { return sim.phys.drogue_deployed; }
bool  sim_get_main_deployed(void)          { return sim.phys.main_deployed; }
bool  sim_get_on_ground(void)              { return sim.phys.on_ground; }

/* ── CLI entry point (not used in WASM) ───────────────────────────── */

#ifndef WASM_BUILD
int main(int argc, char **argv) {
    float target = 1524.0f; /* default: 5000ft */
    if (argc > 1) target = atof(argv[1]);

    const char *config =
        "[pyro]\r\nid=SIM001\r\nname=SimRkt\r\n"
        "pyro1_mode=delay\r\npyro1_value=0\r\n"
        "pyro2_mode=agl\r\npyro2_value=200\r\n"
        "units=ft\r\n";

    sim_init_flight(target, config);

    printf("Simulating %.0fm flight...\n", target);

    int done = 0;
    while (!done) {
        done = sim_tick();

        uint32_t t = hal_time_ms();
        if (t % 1000 == 0 && t > 0) {
            const flight_context_t *ctx = sim_get_ctx();
            printf("t=%5.1fs alt=%7.1fm vel=%6.1fm/s state=%d buzzer=%d",
                   t / 1000.0, sim_get_altitude_m(), sim_get_velocity_ms(),
                   ctx->current_state, sim_get_buzzer_state());
            if (sim_get_drogue_deployed()) printf(" DROGUE");
            if (sim_get_main_deployed()) printf(" MAIN");
            printf("\n");
        }
    }

    const flight_context_t *ctx = sim_get_ctx();
    printf("\n=== Flight Complete ===\n");
    printf("Apogee:     %.0f m (%.0f ft)\n", sim_get_apogee_m(), sim_get_apogee_m() / 0.3048);
    printf("Max alt fw: %ld cm\n", (long)ctx->max_altitude);
    printf("Samples:    %d\n", ctx->buf_count);
    printf("P1 fired:   %s\n", ctx->pyro1_fired ? "yes" : "no");
    printf("P2 fired:   %s\n", ctx->pyro2_fired ? "yes" : "no");
    printf("Telemetry:  %d bytes\n", sim_get_telemetry_len());

    return 0;
}
#endif
