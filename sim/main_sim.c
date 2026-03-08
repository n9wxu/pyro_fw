/*
 * Pyro MK1B simulation entry point.
 *
 * This is the flight computer as a black box. No physics.
 * An external process (JS in browser, C for CLI) drives it:
 *   1. Set inputs (time, pressure, continuity)
 *   2. Call sim_tick()
 *   3. Read outputs (state, pyro fires, buzzer, telemetry)
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "../src/hal.h"
#include "../src/flight_states.h"
#include "../src/buzzer.h"
#include "hal_sim.h"

static flight_context_t ctx;

/* ── API: init ────────────────────────────────────────────────────── */

void sim_flight_init(const char *config_ini) {
    sim_reset();
    if (config_ini && config_ini[0])
        hal_fs_write_file("config.ini", config_ini, strlen(config_ini));
    flight_init(&ctx);
}

/* ── API: tick — call once per millisecond (or per step) ──────────── */

int sim_flight_tick(uint32_t time_ms) {
    sim_set_time(time_ms);
    sim_clear_pyro_firing();
    ctx.current_state = dispatch_state(&ctx, time_ms);
    flight_update_outputs(&ctx, time_ms);
    return ctx.current_state;
}

/* ── API: read outputs ────────────────────────────────────────────── */

int      sim_flight_state(void)         { return ctx.current_state; }
int32_t  sim_flight_altitude_cm(void)   { return ctx.last_altitude; }
int32_t  sim_flight_max_alt_cm(void)    { return ctx.max_altitude; }
int32_t  sim_flight_vspeed_cms(void)    { return ctx.vertical_speed_cms; }
int32_t  sim_flight_pressure(void)      { return ctx.filtered_pressure; }
bool     sim_flight_pyro1_fired(void)   { return ctx.pyro1_fired; }
bool     sim_flight_pyro2_fired(void)   { return ctx.pyro2_fired; }
bool     sim_flight_armed(void)         { return ctx.pyros_armed; }
int      sim_flight_samples(void)       { return ctx.buf_count; }
uint32_t sim_flight_launch_time(void)   { return ctx.launch_time; }

const flight_context_t *sim_flight_ctx(void) { return &ctx; }
