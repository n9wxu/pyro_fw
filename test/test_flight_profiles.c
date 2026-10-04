/*
 * Whole flights, closed loop [TST-03..06]: each pyro mode, from a hop of
 * sixty metres to past the sensor's range, with the plant saying what the
 * rocket really did.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_harness.h"
#include "flight_run.h"
#include "replay.h"

void setUp(void) {}
void tearDown(void) {}

#define AGL_TOL_M 8.0f

static const mp_rocket_t *subsonic(void) {
    return &ROCKETS[SUBSONIC].r;
}

static int pulses_on(const flown_t *r, uint8_t channel) {
    int n = 0;
    for (int i = 0; i < r->pulses; i++)
        n += r->pulse[i].channel == channel;
    return n;
}

static const flown_pulse_t *first_on(const flown_t *r, uint8_t channel) {
    for (int i = 0; i < r->pulses; i++)
        if (r->pulse[i].channel == channel)
            return &r->pulse[i];
    return NULL;
}

/* ── The four modes [PYR-MODE-01..05, TST-04] ─────────────────────── */

void test_PYR_MODE_01_delay_after_apogee(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro1_value=0\npyro2_mode=delay\npyro2_value=4\n",
                             .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 1, 200.0f);
    TEST_ASSERT_TRUE(r.drogue && r.main);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 0.5f, "a delay of 0 is at apogee");
    /* [PYR-MODE-05] From where the rate crossed zero, not from its detection. */
    TEST_ASSERT_FLOAT_WITHIN(0.25f, 4.0f, r.main_t - r.apogee_t);
}

/* [LUA-RUN-02] */
void test_LUA_RUN_02_each_flight_event_is_offered_to_the_script(void) {
    flight_conditions_t c = {
        .config = "pyro1_mode=delay\npyro1_value=0\n", .rate_ms = {20.0f, 0.0f}, .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 3, 400.0f);
    TEST_ASSERT_TRUE(r.drogue);
    const char *launch = strstr(mock_lua_events, "LAUNCH;");
    TEST_ASSERT_NOT_NULL_MESSAGE(launch, mock_lua_events);
    const char *apogee = strstr(launch, "APOGEE;");
    TEST_ASSERT_NOT_NULL_MESSAGE(apogee, mock_lua_events);
    const char *pyro = strstr(apogee, "PYRO1;");
    TEST_ASSERT_NOT_NULL_MESSAGE(pyro, mock_lua_events);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(pyro, "LANDING;"), mock_lua_events);
}

void test_PYR_MODE_02_agl_at_its_height(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=300\n", .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 2, 200.0f);
    TEST_ASSERT_TRUE(r.drogue && r.main);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, 300.0f, r.main_h);
}

/* Both channels on AGL: the first below apogee, the second 60 m under it
 * while the first canopy is still opening. */
void test_PYR_MODE_02_agl_on_both_channels(void) {
    flight_conditions_t c = {.config = "pyro1_mode=agl\npyro1_value=600\npyro2_mode=agl\npyro2_value=540\n",
                             .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 3, 200.0f);
    const flown_pulse_t *p1 = first_on(&r, 1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, 600.0f, p1->h);
    TEST_ASSERT_TRUE_MESSAGE(r.main && r.main_h <= 540.0f + AGL_TOL_M, "pyro 2 is never above its height");
    /* The gap may hold it past its height [PYR-DEPLOY-02]; never by more. */
    TEST_ASSERT_TRUE_MESSAGE(r.main_t - p1->t <= 3.6f || fabsf(r.main_h - 540.0f) <= AGL_TOL_M,
                             "pyro 2 waited longer than the gap");
}

void test_PYR_MODE_03_fallen_from_the_peak(void) {
    flight_conditions_t c = {.config = "pyro1_mode=fallen\npyro1_value=50\npyro2_mode=agl\npyro2_value=200\n",
                             .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 4, 200.0f);
    const flown_pulse_t *p1 = first_on(&r, 1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, r.apogee_h - 50.0f, p1->h);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, 200.0f, r.main_h);
}

void test_PYR_MODE_04_speed_of_descent(void) {
    flight_conditions_t c = {.config = "pyro1_mode=speed\npyro1_value=25\npyro2_mode=agl\npyro2_value=200\n",
                             .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 5, 200.0f);
    const flown_pulse_t *p1 = first_on(&r, 1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 25.0f, -p1->v);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, 200.0f, r.main_h);
}

void test_CFG_04_a_channel_set_to_none_never_fires(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro2_mode=none\nemergency_fire_speed=30\n",
                             .lights_on_pulse = {-1, 0}};
    flown_t r = fly(subsonic(), &ISA, &c, 6, 200.0f);
    TEST_ASSERT_TRUE(pulses_on(&r, 1) >= 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pulses_on(&r, 2), "not even on the emergency fire [PYR-HEALTH-02]");
}

/* [CFG-03] The same trigger entered in feet. */
void test_CFG_03_a_height_in_feet(void) {
    flight_conditions_t c = {.config = "units=ft\npyro1_mode=delay\npyro2_mode=agl\npyro2_value=1000\n",
                             .rate_ms = {20.0f, 6.0f}};
    flown_t r = fly(subsonic(), &ISA, &c, 7, 200.0f);
    TEST_ASSERT_FLOAT_WITHIN(AGL_TOL_M, 304.8f, r.main_h);
}

/* ── One event, both channels [PYR-DEPLOY-01, PYR-DEPLOY-02] ──────── */

void test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=300\n", .rate_ms = {6.0f, 6.0f}};
    flown_t r = fly(&ROCKETS[HOP].r, &ISA, &c, 8, 60.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "apogee %.0f m; drogue %d, main %d", (double)r.apogee_h, r.drogue, r.main);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_h > 31.0f && r.apogee_h < 150.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.main, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t >= r.apogee_t, msg);
    /* A 500 ms pulse and 3 s of quiet between them, and no sooner. */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3.5f, r.main_t - r.drogue_t);
}

/* ── Nothing before apogee [SYS-DEPLOY-03, PYR-SAFE-04, FLT-APO-04] ── */

void test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers(void) {
    /* Triggers already met on the way up: a height above the pad the rocket
     * is below, and a speed it exceeds. */
    flight_conditions_t c = {.config = "pyro1_mode=agl\npyro1_value=5000\npyro2_mode=speed\npyro2_value=1\n"
                                       "emergency_fire_speed=5\n"};
    flown_t r = fly(subsonic(), &ISA, &c, 9, 200.0f);
    TEST_ASSERT_TRUE(r.pulses >= 2);
    for (int i = 0; i < r.pulses; i++)
        TEST_ASSERT_TRUE_MESSAGE(r.pulse[i].t >= r.apogee_t, "a pulse before the true apogee");
    TEST_ASSERT_TRUE_MESSAGE(r.armed && r.armed_t <= r.declared_t, "armed before apogee is declared [FLT-APO-04]");
}

/* ── Arming and the thrust report [FLT-ASC-03..07] ────────────────── */

void test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn(void) {
    flight_conditions_t c = {0};
    flown_t r = fly(subsonic(), &ISA, &c, 10, 200.0f);
    TEST_ASSERT_TRUE(r.armed);
    TEST_ASSERT_TRUE_MESSAGE(r.armed_t > subsonic()->burn_s, "armed while still under thrust");
    TEST_ASSERT_TRUE_MESSAGE(r.armed_t < r.apogee_t, "armed only at apogee");
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("ARMED"));
    TEST_ASSERT_TRUE(harness_log_event_time("ARMED") < harness_log_event_time("APOGEE"));
}

void test_FLT_ASC_03_the_thrust_report_ends_within_a_second_of_burnout(void) {
    const int rockets[] = {SUBSONIC, MID_MACH, DRAGGY, LOW_DRAG};
    for (unsigned i = 0; i < 4; i++) {
        flight_conditions_t c = {.stop_after_apogee = true};
        const mp_rocket_t *rk = &ROCKETS[rockets[i]].r;
        flown_t r = fly(rk, &ISA, &c, 11, 200.0f);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: thrust report ended %.2f s after burnout", ROCKETS[rockets[i]].name,
                 (double)(r.thrust_end_t - rk->burn_s));
        TEST_ASSERT_TRUE_MESSAGE(r.thrust_end_t >= rk->burn_s - 0.05f && r.thrust_end_t - rk->burn_s <= 1.0f, msg);
    }
}

/* ── Apogee at every height [FLT-APO-01, TST-05] ──────────────────── */

typedef struct {
    int rocket;
    float late_s; /* the latest apogee may be declared after the true one */
} apogee_case_t;

void test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height(void) {
    const apogee_case_t cases[] = {{HOP, 0.5f}, {SUBSONIC, 0.5f}, {LOW_DRAG, 0.5f}, {TO_20_KM, 1.5f}, {TO_30_KM, 2.5f}};
    const float noises[] = {SENSOR_RMS_PA, 3.0f, NOISY_SENSOR_RMS_PA};
    char bad[512] = "";
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        float worst = 0.0f;
        for (unsigned n = 0; n < 3; n++) {
            for (uint32_t seed = 1; seed <= 4; seed++) {
                flight_conditions_t c = {.sensor_rms_pa = noises[n], .thin_air = true};
                const mp_rocket_t *rk = &ROCKETS[cases[k].rocket].r;
                flown_t r = fly(rk, &ISA, &c, seed, plant_apogee_s(rk, &ISA) + 6.0f);
                float late = r.apogee_declared ? r.declared_t - r.apogee_t : 1e6f;
                worst = late > worst ? late : worst;
                if (late < 0.0f || late > cases[k].late_s) {
                    char item[80];
                    snprintf(item, sizeof(item), " %s at %.1f Pa seed %u: %+.2f s;", ROCKETS[cases[k].rocket].name,
                             (double)noises[n], (unsigned)seed, (double)late);
                    strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
                }
            }
        }
        printf("  %-9s apogee declared at most %.2f s after the true one\n", ROCKETS[cases[k].rocket].name,
               (double)worst);
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [SNS-MAX-01, TST-05] Past the sensor's range the flight goes on with the
 * data it has: apogee after the true one, both channels out, a landing. */
void test_SNS_MAX_01_best_effort_above_the_sensors_range(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=300\n",
                             .rate_ms = {20.0f, 6.0f},
                             .thin_air = true,
                             .to_landed = true,
                             .sensor_rms_pa = 3.0f};
    flown_t r = fly(&ROCKETS[TO_45_KM].r, &ISA, &c, 12, 4000.0f);
    char msg[160];
    snprintf(msg, sizeof(msg), "apogee %.0f m; declared %+.1f s after; drogue %d main %d at %.0f m; state %d",
             (double)r.apogee_h, (double)(r.declared_t - r.apogee_t), r.drogue, r.main, (double)r.main_h,
             r.final_state);
    printf("  45 km: %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_h > 40000.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_declared && r.declared_t >= r.apogee_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.main, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(LANDED, r.final_state, msg);
}

/* [SNS-ALT-01] Nothing is clamped: a peak above 8000 m is reported as it
 * is, and a landing below the pad is a negative altitude. */
void test_SNS_ALT_01_the_reported_peak_is_not_clamped(void) {
    flight_conditions_t c = {.stop_after_apogee = true};
    flown_t r = fly(&ROCKETS[LOW_DRAG].r, &ISA, &c, 13, 200.0f);
    TEST_ASSERT_TRUE(r.apogee_h > 9000.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * r.apogee_h, r.apogee_h, (float)r.peak_cm / 100.0f);
}

void test_SNS_ALT_01_below_the_pad_is_a_negative_altitude(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 0.0f}, .to_landed = true};
    flown_t r = fly(&ROCKETS[HOP].r, &ISA, &c, 13, 120.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    /* Carried 50 m down the hill from where it landed. */
    mock_pressure.pressure_pa = mp_pad_pa(&ISA) + 600.0f;
    uint32_t t = mock_time_ms + 1;
    for (uint32_t end = t + 4000u; t < end; t++)
        tick(t);
    TEST_ASSERT_INT32_WITHIN(300, -5000, ctx.altitude_cm);
}

/* ── Descent, landing and the log [FLT-DESC-01/02, FLT-LAND-02..07] ── */

void test_TST_06_a_canopy_slows_the_descent(void) {
    flight_conditions_t with = {.rate_ms = {20.0f, 0.0f}, .to_landed = true};
    flight_conditions_t without = {.lights_on_pulse = {-1, 0}, .to_landed = true};
    flown_t a = fly(subsonic(), &ISA, &with, 14, 300.0f);
    flown_t b = fly(subsonic(), &ISA, &without, 14, 300.0f);
    TEST_ASSERT_TRUE(a.fastest_descent_ms < 25.0f);
    TEST_ASSERT_TRUE(b.fastest_descent_ms > 60.0f);
    TEST_ASSERT_TRUE(a.touchdown_t > b.touchdown_t + 10.0f);
}

/* A flight that deploys nothing still lands, and closes its log. */
void test_FLT_DESC_02_a_ballistic_flight_reaches_landed(void) {
    flight_conditions_t c = {.config = "pyro1_mode=none\npyro2_mode=none\n", .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 15, 300.0f);
    TEST_ASSERT_EQUAL_INT(0, r.pulses);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    TEST_ASSERT_TRUE_MESSAGE(r.landed_t - r.touchdown_t <= 3.0f, "LANDED within 3 s of touchdown");
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("LANDING"));
}

/* [FLT-DESC-01] The phase is the rate's, not the channel's: with the
 * channels the other way round -- pyro 2 at apogee on the fast canopy, pyro 1
 * low on the slow one -- the telemetry reads the same phases. */
void test_FLT_DESC_01_the_phase_follows_the_rate_not_the_channel(void) {
    flight_conditions_t c = {.config = "pyro2_mode=delay\npyro2_value=0\npyro1_mode=agl\npyro1_value=300\n",
                             .rate_ms = {6.0f, 20.0f},
                             .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 16, 300.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    /* $PYRO,seq,state,...: state 3 is the drogue's rate band, 4 the main's. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, ",3,0,"), "no sentence in the drogue-rate phase");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, ",4,0,"), "no sentence in the main-rate phase");
}

/* [FLT-LAND-02] A main at 6 m/s is not a landing: LANDED comes at touchdown,
 * not on the way down. */
void test_FLT_LAND_02_a_slow_descent_is_not_a_landing(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 6.0f}, .main_m = 200, .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 17, 300.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    TEST_ASSERT_TRUE_MESSAGE(r.landed_t >= r.touchdown_t, "LANDED before the rocket was down");
    TEST_ASSERT_TRUE(r.landed_t - r.touchdown_t <= 3.0f);
}

/* ── The log [DAT-02..04, DAT-06..08, FLT-LAUNCH-03, FLT-LAUNCH-04, GND-CAL-05] ── */

void test_DAT_04_the_events_of_a_flight_in_order(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 6.0f}, .main_m = 200, .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 18, 300.0f);
    (void)r;
    const char *order[] = {"LAUNCH", "ARMED", "APOGEE", "PYRO1", "PYRO2", "LANDING"};
    float last = -1.0f;
    for (unsigned i = 0; i < 6; i++) {
        float t = harness_log_event_time(order[i]);
        TEST_ASSERT_TRUE_MESSAGE(harness_log_events(order[i]) >= 1, order[i]);
        TEST_ASSERT_TRUE_MESSAGE(t >= last, order[i]);
        last = t;
    }
}

/* [FLT-LAUNCH-03, GND-CAL-05] The log's time is since the rocket left the
 * pad: the LAUNCH row is at the detection, a second or so into the flight,
 * and at the height reached by then, not zero. */
void test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise(void) {
    flight_conditions_t c = {.stop_after_apogee = true};
    flown_t r = fly(subsonic(), &ISA, &c, 19, 200.0f);
    float logged = harness_log_event_time("LAUNCH");
    char msg[96];
    snprintf(msg, sizeof(msg), "declared %.2f s after ignition at %.0f m; the log's LAUNCH row is at %.2f s",
             (double)r.launch_t, (double)r.launch_h, (double)logged);
    TEST_ASSERT_TRUE_MESSAGE(r.launch_h > 30.48f && r.launch_h < 60.0f, msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, r.launch_t, logged, msg);
}

/* [DAT-08] A full-rate log, replayed through the firmware, decides the same
 * flight. */
void test_DAT_08_a_full_rate_log_replays_to_the_same_events(void) {
    flight_conditions_t c = {.config = "log_rate=full\npyro1_mode=delay\npyro2_mode=agl\npyro2_value=40\n",
                             .rate_ms = {12.0f, 5.0f},
                             .to_landed = true};
    flown_t r = fly(&ROCKETS[HOP].r, &ISA, &c, 20, 120.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    static char csv[4 * 1024 * 1024];
    int n = test_flight_log_csv(csv, (int)sizeof(csv));
    TEST_ASSERT_TRUE(n > 0);
    replay_events_t logged, decided;
    TEST_ASSERT_TRUE(replay_logged_events(csv, &logged));
    boot_like_hardware(20);
    TEST_ASSERT_TRUE_MESSAGE(replay_run(csv, &decided), "the replay did not run");
    char msg[160];
    snprintf(msg, sizeof(msg), "apogee %u/%u pyro1 %u/%u pyro2 %u/%u landing %u/%u ms (logged/replayed)",
             (unsigned)logged.apogee_ms, (unsigned)decided.apogee_ms, (unsigned)logged.pyro1_ms,
             (unsigned)decided.pyro1_ms, (unsigned)logged.pyro2_ms, (unsigned)decided.pyro2_ms,
             (unsigned)logged.landing_ms, (unsigned)decided.landing_ms);
    printf("  replay: %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(logged.apogee_ms && logged.pyro1_ms && logged.pyro2_ms && logged.landing_ms, msg);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(300, logged.apogee_ms, decided.apogee_ms, msg);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(300, logged.pyro1_ms, decided.pyro1_ms, msg);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(500, logged.pyro2_ms, decided.pyro2_ms, msg);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(1000, logged.landing_ms, decided.landing_ms, msg);
}

void test_DAT_08_a_thinned_log_is_refused(void) {
    flight_conditions_t c = {.config = "log_rate=1hz\n", .rate_ms = {20.0f, 0.0f}, .to_landed = true};
    (void)fly(subsonic(), &ISA, &c, 21, 300.0f);
    static char csv[1024 * 1024];
    TEST_ASSERT_TRUE(test_flight_log_csv(csv, (int)sizeof(csv)) > 0);
    replay_events_t decided;
    boot_like_hardware(21);
    TEST_ASSERT_FALSE(replay_run(csv, &decided));
}

/* ── Processing lateness changes nothing [FLT-RATE-05] ────────────── */

static uint32_t late_loop(uint32_t t) {
    return (t / 700u) % 2u ? 60u : 0u; /* the loop clock, 60 ms late for 0.7 s at a time */
}

void test_FLT_RATE_05_a_late_loop_changes_no_decision(void) {
    flight_conditions_t on_time = {.rate_ms = {20.0f, 6.0f}, .main_m = 200};
    flight_conditions_t late = on_time;
    late.loop_lag = late_loop;
    flown_t a = fly(subsonic(), &ISA, &on_time, 22, 200.0f);
    flown_t b = fly(subsonic(), &ISA, &late, 22, 200.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.07f, a.declared_t, b.declared_t);
    TEST_ASSERT_FLOAT_WITHIN(0.07f, a.drogue_t, b.drogue_t);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, a.main_h, b.main_h);
}

/* [FLT-RT-01] The flight software held for up to 73 ms at a time, as storage
 * and the network hold it: every decision still comes, within the bound, and
 * each pulse is still commanded. */
void test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound(void) {
    flight_conditions_t quiet = {.rate_ms = {20.0f, 6.0f}, .main_m = 200};
    flight_conditions_t busy = quiet;
    busy.stalls = true;
    flown_t a = fly(subsonic(), &ISA, &quiet, 23, 200.0f);
    flown_t b = fly(subsonic(), &ISA, &busy, 23, 200.0f);
    TEST_ASSERT_TRUE_MESSAGE(mock_stall_count > 50u, "the flight was held, often");
    TEST_ASSERT_TRUE_MESSAGE(b.declared_t >= b.apogee_t, "never early");
    TEST_ASSERT_FLOAT_WITHIN(0.25f, a.declared_t, b.declared_t);
    TEST_ASSERT_TRUE_MESSAGE(b.drogue_t - b.declared_t <= 0.25f, "the pulse follows its decision within the bound");
    TEST_ASSERT_FLOAT_WITHIN(20.0f * 0.25f, a.main_h, b.main_h);
    TEST_ASSERT_EQUAL_INT(a.pulses, b.pulses);
}

/* ── After the landing [BUZ-03, SYS-DATA-03, USB-02, USB-04] ──────── */

static void stay_landed(uint32_t ms) {
    for (uint32_t t = mock_time_ms + 1, end = t + ms; t < end; t++)
        tick(t);
}

void test_BUZ_03_the_peak_is_beeped_out_after_landing(void) {
    flight_conditions_t metres = {.to_landed = true};
    flown_t r = fly(&ROCKETS[HOP].r, &ISA, &metres, 24, 300.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    TEST_ASSERT_EQUAL_INT(1, harness_altitude_plays);
    TEST_ASSERT_FLOAT_WITHIN(0.03f * r.apogee_h + 1.0f, r.apogee_h, (float)harness_last_altitude);

    flight_conditions_t feet = {.config = "units=ft\n", .to_landed = true};
    r = fly(&ROCKETS[HOP].r, &ISA, &feet, 24, 300.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f * r.apogee_h * 3.2808f + 3.0f, r.apogee_h * 3.2808f,
                                     (float)harness_last_altitude, "in the configured units [BUZ-04]");
}

/* A board picked up and plugged in stops beeping the altitude, and takes it
 * up again when unplugged. */
void test_BUZ_03_the_beep_out_is_held_on_usb_and_resumes(void) {
    flight_conditions_t c = {.to_landed = true};
    fly(&ROCKETS[HOP].r, &ISA, &c, 25, 300.0f);
    stay_landed(3000);
    TEST_ASSERT_EQUAL_INT(1, harness_altitude_plays);
    harness_usb(true);
    stay_landed(3000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_usb_ok_plays, "the attach chirp takes the buzzer");
    TEST_ASSERT_EQUAL_INT(1, harness_altitude_plays);
    TEST_ASSERT_EQUAL_INT_MESSAGE(LANDED, ctx.current_state, "and the flight's record stays");
    harness_usb(false);
    stay_landed(1000);
    TEST_ASSERT_EQUAL_INT(2, harness_altitude_plays);
}

/* ── Storage in flight [WEB-API-08, WEB-API-10, DD-058] ───────────── */

/* From launch until the record is safe the flight software stores nothing
 * but the flight's record: a whole flight asks for no other file. */
void test_WEB_API_08_only_the_flight_record_is_stored_in_flight(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 6.0f}, .main_m = 200, .to_landed = true, .pad_s = 12.0f};
    flown_t r = fly(subsonic(), &ISA, &c, 27, 300.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mock_fs_locked_count, "the flight software asked for another file");
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "the resume record is cleared once the log is closed");
}

/* Anything else that asks is refused while the log is being written, and
 * served again once the flight has landed. */
void test_WEB_API_10_other_storage_access_is_refused_until_the_record_is_safe(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 6.0f}, .stop_after_apogee = true};
    fly(subsonic(), &ISA, &c, 28, 300.0f);
    char buf[32];
    TEST_ASSERT_EQUAL_INT(HAL_FS_LOCKED, hal_fs_write_file("upload.bin", "x", 1));
    TEST_ASSERT_EQUAL_INT(HAL_FS_LOCKED, hal_fs_read_file("config.ini", buf, (int)sizeof(buf)));
    TEST_ASSERT_NULL(hal_fs_open("upload.bin", false));

    flight_conditions_t landed = {.rate_ms = {20.0f, 6.0f}, .to_landed = true};
    fly(&ROCKETS[HOP].r, &ISA, &landed, 28, 300.0f);
    stay_landed(1500);
    TEST_ASSERT_TRUE(hal_fs_write_file("upload.bin", "x", 1) != HAL_FS_LOCKED);
    TEST_ASSERT_TRUE(hal_fs_read_file("config.ini", buf, (int)sizeof(buf)) > 0);
}

/* ── A failed sensor is not recovered in flight [SNS-REC-01] ──────── */

void test_SNS_REC_01_a_failed_sensor_is_logged_and_left_alone(void) {
    float coast = 0.5f * (subsonic()->burn_s + plant_apogee_s(subsonic(), &ISA));
    flight_conditions_t c = {.rate_ms = {20.0f, 6.0f}, .main_m = 200, .stuck_at_s = coast, .stuck_s = 3.0f};
    flown_t r = fly(subsonic(), &ISA, &c, 26, 200.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_pressure_inits, "initialised at start-up and never again");
    TEST_ASSERT_TRUE(harness_log_events("SENSOR_STUCK") >= 1);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_declared && r.drogue && r.main,
                             "the flight goes on with the samples that arrive");
    TEST_ASSERT_TRUE(r.drogue_t >= r.apogee_t);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PYR_MODE_01_delay_after_apogee);
    RUN_TEST(test_LUA_RUN_02_each_flight_event_is_offered_to_the_script);
    RUN_TEST(test_PYR_MODE_02_agl_at_its_height);
    RUN_TEST(test_PYR_MODE_02_agl_on_both_channels);
    RUN_TEST(test_PYR_MODE_03_fallen_from_the_peak);
    RUN_TEST(test_PYR_MODE_04_speed_of_descent);
    RUN_TEST(test_CFG_04_a_channel_set_to_none_never_fires);
    RUN_TEST(test_CFG_03_a_height_in_feet);
    RUN_TEST(test_PYR_DEPLOY_01_a_low_flight_puts_both_out_on_one_event);
    RUN_TEST(test_PYR_SAFE_04_no_fire_before_apogee_whatever_the_triggers);
    RUN_TEST(test_FLT_ASC_04_arms_in_the_coast_not_in_the_burn);
    RUN_TEST(test_FLT_ASC_03_the_thrust_report_ends_within_a_second_of_burnout);
    RUN_TEST(test_FLT_APO_01_apogee_is_never_early_and_soon_after_at_every_height);
    RUN_TEST(test_SNS_MAX_01_best_effort_above_the_sensors_range);
    RUN_TEST(test_SNS_ALT_01_the_reported_peak_is_not_clamped);
    RUN_TEST(test_SNS_ALT_01_below_the_pad_is_a_negative_altitude);
    RUN_TEST(test_TST_06_a_canopy_slows_the_descent);
    RUN_TEST(test_FLT_DESC_02_a_ballistic_flight_reaches_landed);
    RUN_TEST(test_FLT_DESC_01_the_phase_follows_the_rate_not_the_channel);
    RUN_TEST(test_FLT_LAND_02_a_slow_descent_is_not_a_landing);
    RUN_TEST(test_DAT_04_the_events_of_a_flight_in_order);
    RUN_TEST(test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise);
    RUN_TEST(test_DAT_08_a_full_rate_log_replays_to_the_same_events);
    RUN_TEST(test_DAT_08_a_thinned_log_is_refused);
    RUN_TEST(test_FLT_RATE_05_a_late_loop_changes_no_decision);
    RUN_TEST(test_FLT_RT_01_other_activity_delays_no_decision_past_the_bound);
    RUN_TEST(test_BUZ_03_the_peak_is_beeped_out_after_landing);
    RUN_TEST(test_BUZ_03_the_beep_out_is_held_on_usb_and_resumes);
    RUN_TEST(test_WEB_API_08_only_the_flight_record_is_stored_in_flight);
    RUN_TEST(test_WEB_API_10_other_storage_access_is_refused_until_the_record_is_safe);
    RUN_TEST(test_SNS_REC_01_a_failed_sensor_is_logged_and_left_alone);
    return UNITY_END();
}
