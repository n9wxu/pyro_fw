/*
 * CLI simulation driver — physics engine that drives the pyro black box.
 *
 * This is NOT part of the pyro firmware. It's a separate process
 * that feeds inputs and reads outputs from the flight computer.
 *
 * Usage: ./pyro_sim_cli [altitude_m]
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/flight_states.h"
#include "../src/hal.h"
#include "hal_sim.h"

/* ── Pyro black box API (defined in main_sim.c) ──────────────────── */

void sim_flight_init(const char *config_ini);
int  sim_flight_tick(uint32_t time_ms);
int  sim_flight_state(void);
int32_t sim_flight_max_alt_cm(void);
int  sim_flight_samples(void);
bool sim_flight_pyro1_fired(void);
bool sim_flight_pyro2_fired(void);
const flight_context_t *sim_flight_ctx(void);
void sim_flight_save_csv(void);

/* ── Physics (entirely outside the pyro code) ─────────────────────── */

#define G           9.81f
#define DT          0.001f
#define GROUND_PA   101325.0f
#define PAD_DWELL_MS 2000

#define DROGUE_DRAG  0.8f
#define MAIN_DRAG    4.0f

typedef struct {
    float alt_m, vel_ms;
    float thrust_accel, burn_time;
    bool drogue, main_chute, on_ground;
} physics_t;

static float alt_to_pa(float alt_m) {
    if (alt_m < 11000.0f)
        return GROUND_PA * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
    float p11 = GROUND_PA * powf(1.0f - 0.0065f * 11000.0f / 288.15f, 5.2561f);
    if (alt_m < 47000.0f)
        return p11 * expf(-9.81f * (alt_m - 11000.0f) / (287.05f * 216.65f));
    float p47 = p11 * expf(-9.81f * 36000.0f / (287.05f * 216.65f));
    return p47 * expf(-9.81f * (alt_m - 47000.0f) / (287.05f * 270.65f));
}

static void physics_step(physics_t *p, float flight_t) {
    if (p->on_ground) return;
    float a = -G;
    if (flight_t < p->burn_time) a += p->thrust_accel;
    if (p->vel_ms < 0.0f) {
        float drag = 0.05f;
        if (p->main_chute) drag = MAIN_DRAG;
        else if (p->drogue) drag = DROGUE_DRAG;
        a += drag * expf(-p->alt_m / 8500.0f) * (-p->vel_ms);
    }
    p->vel_ms += a * DT;
    p->alt_m += p->vel_ms * DT;
    if (p->alt_m <= 0.0f) { p->alt_m = 0; p->vel_ms = 0; p->on_ground = true; }
}

static void compute_burn(physics_t *p, float target_m) {
    if (target_m > 50000.0f)      p->thrust_accel = 5.0f * G;
    else if (target_m > 500.0f)   p->thrust_accel = 10.0f * G;
    else                          p->thrust_accel = 20.0f * G;
    float lo = 0, hi = 200;
    for (int i = 0; i < 50; i++) {
        float t = (lo + hi) / 2.0f;
        float a = p->thrust_accel - G;
        float h = 0.5f * a * t * t + (a * t) * (a * t) / (2.0f * G);
        if (h < target_m) lo = t; else hi = t;
    }
    p->burn_time = (lo + hi) / 2.0f;
}

/* ── Main: drive the simulation ───────────────────────────────────── */

int main(int argc, char **argv) {
    float target = 1524.0f;
    if (argc > 1) target = atof(argv[1]);

    /* Initialize physics */
    physics_t phys = {0};
    compute_burn(&phys, target);
    float apogee = 0;

    /* Initialize pyro black box */
    const char *config =
        "[pyro]\r\nid=SIM001\r\nname=SimRkt\r\n"
        "pyro1_mode=delay\r\npyro1_value=0\r\n"
        "pyro2_mode=agl\r\npyro2_value=200\r\n"
        "units=ft\r\n";
    sim_flight_init(config);

    uint32_t step = (target > 50000.0f) ? 50 : 1;
    uint32_t max_ms = (target > 50000.0f) ? 15000000
                    : (target > 500.0f)   ? 600000 : 120000;
    int prev_fires = 0;

    printf("Simulating %.0fm (%.0fft) flight...\n", target, target / 0.3048f);

    for (uint32_t t = 0; t <= max_ms; t += step) {

        /* 1. Read pyro outputs → update physics */
        int fires = sim_get_pyro_fire_count();
        if (fires > prev_fires) {
            uint8_t ch = sim_get_pyro_last_channel();
            if (ch == 1 && !phys.drogue) phys.drogue = true;
            if (ch == 2 && !phys.main_chute) phys.main_chute = true;
            prev_fires = fires;
        }

        /* 2. Step physics */
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            for (uint32_t s = 0; s < step; s++)
                physics_step(&phys, ft + (float)s * DT);
        }
        if (phys.alt_m > apogee) apogee = phys.alt_m;

        /* 3. Feed inputs to pyro black box */
        sim_set_pressure(alt_to_pa(phys.alt_m));

        /* 4. Tick the flight computer */
        int state = sim_flight_tick(t);

        /* 5. Print status */
        if (t % 1000 == 0 && t > 0) {
            printf("t=%5.1fs alt=%7.1fm vel=%6.1fm/s state=%d buz=%d",
                   t / 1000.0, phys.alt_m, phys.vel_ms, state, sim_get_buzzer_state());
            if (phys.drogue) printf(" DROGUE");
            if (phys.main_chute) printf(" MAIN");
            printf("\n");
        }

        if (state == LANDED) break;
    }

    printf("\n=== Flight Complete ===\n");
    printf("Apogee:     %.0f m (%.0f ft)\n", apogee, apogee / 0.3048);
    printf("Max alt fw: %ld cm\n", (long)sim_flight_max_alt_cm());
    printf("Samples:    %d\n", sim_flight_samples());
    printf("P1 fired:   %s\n", sim_flight_pyro1_fired() ? "yes" : "no");
    printf("P2 fired:   %s\n", sim_flight_pyro2_fired() ? "yes" : "no");
    printf("Telemetry:  %d bytes\n", sim_get_telemetry_len());

    /* Save and display CSV */
    sim_flight_save_csv();
    char csv[512];
    int n = hal_fs_read_file("flight.csv", csv, sizeof(csv) - 1);
    if (n > 0) {
        csv[n] = '\0';
        printf("\n=== CSV (first %d bytes) ===\n%s...\n", n, csv);
    }

    return 0;
}
