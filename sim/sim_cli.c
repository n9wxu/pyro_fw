/*
 * The command-line simulator: sim/physics.c flies the rocket, and the flight
 * computer (sim/pyro_sim.h) flies against its pressure.
 *
 * Usage: pyro_sim [apogee_m]
 *        pyro_sim --replay <flight_log.csv>
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "physics.h"
#include "pyro_sim.h"
#include "replay.h"

#include "../src/flight_states.h"
#include "../src/hal.h"

/* Long enough for the boot sequence to finish and the 5 s ground reference
 * to fill: ignite earlier and the board calibrates on the way up. */
#define PAD_DWELL_MS 9000

/* A replay reads the whole log into memory. */
#define REPLAY_MAX_BYTES (1u << 20)

/* The replay drives the HAL's clock. */
static void set_clock(uint32_t now_ms) {
    sim_set_time(now_ms);
}

/* pyro_sim --replay <flight_log.csv>: the log's readings through the firmware,
 * its decisions against the log's own [DAT-02]. */
static int replay_file(const char *path) {
    replay_row_hook = set_clock;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    static char csv[REPLAY_MAX_BYTES + 1];
    size_t n = fread(csv, 1, REPLAY_MAX_BYTES, f);
    bool truncated = n == REPLAY_MAX_BYTES && fgetc(f) != EOF;
    fclose(f);
    if (truncated) {
        printf("%s: longer than %u bytes; replay a shorter log\n", path, REPLAY_MAX_BYTES);
        return 1;
    }
    csv[n] = '\0';
    replay_events_t logged, decided;
    if (!replay_logged_events(csv, &logged) || !replay_run(csv, &decided)) {
        printf("%s: not a flight log with a raw_pa column, or logged at a row a second\n"
               "(set log_rate=full in config.ini to log a replayable flight)\n",
               path);
        return 1;
    }
    printf("%d sample rows replayed\n", decided.rows);
    printf("%-8s %10s %10s\n", "event", "logged", "replayed");
    printf("%-8s %10u %10u\n", "apogee", (unsigned)logged.apogee_ms, (unsigned)decided.apogee_ms);
    printf("%-8s %10u %10u\n", "pyro1", (unsigned)logged.pyro1_ms, (unsigned)decided.pyro1_ms);
    printf("%-8s %10u %10u\n", "pyro2", (unsigned)logged.pyro2_ms, (unsigned)decided.pyro2_ms);
    printf("%-8s %10u %10u\n", "landing", (unsigned)logged.landing_ms, (unsigned)decided.landing_ms);
    if (decided.diverged_ms)
        printf("first state that differs from the log: at %u ms\n", (unsigned)decided.diverged_ms);
    return 0;
}

#define DEFAULT_APOGEE_M 1524.0f
#define HIGH_FLIGHT_M 50000.0f

static int usage(void) {
    printf("usage: pyro_sim [apogee_m]\n       pyro_sim --replay <flight_log.csv>\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--replay") == 0)
        return argc == 3 ? replay_file(argv[2]) : usage();
    float target = DEFAULT_APOGEE_M;
    if (argc > 1) {
        char *end;
        target = strtof(argv[1], &end);
        if (argc > 2 || *end != '\0' || !(target > 0.0f))
            return usage();
    }

    physics_state_t phys;
    physics_init(&phys, target);

    const char *config = "[pyro]\r\nid=SIM001\r\nname=SimRkt\r\n"
                         "pyro1_mode=delay\r\npyro1_value=0\r\n"
                         "pyro2_mode=agl\r\npyro2_value=200\r\n"
                         "units=ft\r\n";
    sim_flight_init(config);

    uint32_t step = (target > HIGH_FLIGHT_M) ? 50 : 1;
    uint32_t max_ms = (target > HIGH_FLIGHT_M) ? 15000000 : (target > 500.0f) ? 600000 : 120000;
    int prev_fires = 0;

    printf("Simulating %.0fm (%.0fft) flight...\n", target, target / 0.3048f);

    for (uint32_t t = 0; t <= max_ms; t += step) {
        int fires = sim_get_pyro_fire_count();
        if (fires > prev_fires) {
            uint8_t ch = sim_get_pyro_last_channel();
            if (ch == 1)
                physics_deploy_drogue(&phys);
            if (ch == 2)
                physics_deploy_main(&phys);
            prev_fires = fires;
        }

        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            for (uint32_t s = 0; s < step; s++)
                physics_step(&phys, ft + (float)s * PHYS_STEP_S);
        }
        sim_set_pressure(physics_pressure_pa(phys.alt_m));
        int state = sim_flight_tick(t);

        if (t % 1000 == 0 && t > 0) {
            printf("t=%5.1fs alt=%7.1fm vel=%6.1fm/s state=%d buz=%d", t / 1000.0, phys.alt_m, phys.vel_ms, state,
                   sim_get_buzzer_state());
            if (phys.drogue_deployed)
                printf(" DROGUE");
            if (phys.main_deployed)
                printf(" MAIN");
            printf("\n");
        }

        if (state == LANDED)
            break;
    }

    printf("\n=== Flight Complete ===\n");
    printf("Apogee:     %.0f m (%.0f ft)\n", phys.apogee_m, phys.apogee_m / 0.3048);
    printf("Max alt fw: %ld cm\n", (long)sim_flight_max_alt_cm());
    printf("Samples:    %d\n", sim_flight_samples());
    printf("P1 fired:   %s\n", sim_flight_pyro1_fired() ? "yes" : "no");
    printf("P2 fired:   %s\n", sim_flight_pyro2_fired() ? "yes" : "no");
    printf("Telemetry:  %d bytes\n", sim_get_telemetry_len());

    sim_flight_save_csv();
    char csv[512];
    int n = hal_fs_read_file("flight.csv", csv, sizeof(csv) - 1);
    if (n > 0) {
        csv[n] = '\0';
        printf("\n=== CSV (first %d bytes) ===\n%s...\n", n, csv);
    }

    return 0;
}
