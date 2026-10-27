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
#include <limits.h>
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
    /* Up to the ceiling, and the low-drag rocket's 9.5 km just past it. */
    const apogee_case_t cases[] = {{HOP, 0.5f}, {SUBSONIC, 0.5f}, {MID_MACH, 0.5f}, {LOW_DRAG, 0.5f}};
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

/* [FLT-APO-01, PYR-MODE-06] Another board in the airframe fires at the top,
 * as a backup altimeter does: its separation shocks the readings and the
 * drogue opens. Apogee is still never early and soon after. */
void test_FLT_APO_01_another_boards_separation_at_the_top_does_not_delay_apogee(void) {
    const mp_shock_t shocks[] = {{1000.0f, 0.02f}, {-1000.0f, 0.02f}, {5000.0f, 0.04f}, {-5000.0f, 0.04f}};
    const float after_s[] = {0.0f, 0.15f, 0.3f};
    char bad[512] = "";
    float worst = 0.0f;
    for (unsigned k = 0; k < sizeof(shocks) / sizeof(shocks[0]); k++)
        for (unsigned a = 0; a < sizeof(after_s) / sizeof(after_s[0]); a++)
            for (uint32_t seed = 1; seed <= 3; seed++) {
                flight_conditions_t c = {.foreign = shocks[k], .foreign_after_s = after_s[a]};
                flown_t r = fly(subsonic(), &ISA, &c, seed, 60.0f);
                float late = r.declared_t - r.apogee_t;
                if (r.apogee_declared && late > worst)
                    worst = late;
                if (!r.apogee_declared || late < 0.0f || late > 0.5f) {
                    char item[64];
                    snprintf(item, sizeof(item), " %+.0f Pa at +%.2f s/%u: %+.2f s;", (double)shocks[k].peak_pa,
                             (double)after_s[a], (unsigned)seed, (double)late);
                    strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
                }
            }
    printf("  another board's separation at the top: apogee within +%.2f s%s%s\n", (double)worst,
           bad[0] ? "; wrong:" : "", bad);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-APO-12] The user's lockout holds apogee until it has run from T+0,
 * which is a little after the flight's start. */
void test_FLT_APO_12_apogee_waits_for_the_users_lockout(void) {
    flown_t free = fly(subsonic(), &ISA, &(flight_conditions_t){0}, 1, 60.0f);
    TEST_ASSERT_TRUE(free.apogee_declared);
    char ini[48];
    unsigned lockout_s = (unsigned)free.declared_t + 3u;
    snprintf(ini, sizeof(ini), "apogee_lockout=%u\n", lockout_s);
    flown_t held = fly(subsonic(), &ISA, &(flight_conditions_t){.config = ini}, 1, 60.0f);
    TEST_ASSERT_TRUE(held.apogee_declared);
    TEST_ASSERT_TRUE(held.declared_t >= (float)lockout_s);
    TEST_ASSERT_TRUE(held.declared_t < held.launch_t + (float)lockout_s);
    TEST_ASSERT_TRUE(held.drogue);
}

/* [SNS-MAX-01, TST-05] Above the ceiling apogee is still declared, and never
 * before the true one. */
void test_SNS_MAX_01_above_the_ceiling_apogee_is_never_early(void) {
    const int rockets[] = {TO_20_KM, TO_30_KM};
    char bad[256] = "";
    for (unsigned k = 0; k < 2; k++) {
        for (uint32_t seed = 1; seed <= 4; seed++) {
            flight_conditions_t c = {.sensor_rms_pa = NOISY_SENSOR_RMS_PA, .thin_air = true};
            const mp_rocket_t *rk = &ROCKETS[rockets[k]].r;
            flown_t r = fly(rk, &ISA, &c, seed, plant_apogee_s(rk, &ISA) + 10.0f);
            if (!r.apogee_declared || r.declared_t < r.apogee_t) {
                char item[64];
                snprintf(item, sizeof(item), " %s seed %u: declared %d at %+.2f s;", ROCKETS[rockets[k]].name,
                         (unsigned)seed, r.apogee_declared, (double)(r.declared_t - r.apogee_t));
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
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
    TEST_ASSERT_TRUE_MESSAGE(r.landed_t - r.touchdown_t <= 4.0f, "LANDED within 4 s of touchdown");
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
 * pad: the LAUNCH row is at the detection, half a second or so into the
 * flight, and at the height reached by then, not zero. */
void test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise(void) {
    flight_conditions_t c = {.stop_after_apogee = true};
    flown_t r = fly(subsonic(), &ISA, &c, 19, 200.0f);
    float logged = harness_log_event_time("LAUNCH");
    char msg[96];
    snprintf(msg, sizeof(msg), "declared %.2f s after ignition at %.0f m; the log's LAUNCH row is at %.2f s",
             (double)r.launch_t, (double)r.launch_h, (double)logged);
    TEST_ASSERT_TRUE_MESSAGE(r.launch_h > 15.0f && r.launch_h < 30.0f, msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, r.launch_t, logged, msg);
}

/* [FLT-LOG-07] Every sample from launch to landing is a row, and every event
 * row sits at its own time, in time order. */
void test_FLT_LOG_07_every_sample_is_a_row(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 0.0f}, .to_landed = true};
    flown_t r = fly(subsonic(), &ISA, &c, 23, 300.0f);
    TEST_ASSERT_EQUAL_INT(LANDED, r.final_state);
    static char csv[4 * 1024 * 1024];
    TEST_ASSERT_TRUE(test_flight_log_csv(csv, (int)sizeof(csv)) > 0);
    const char *p = strstr(csv, "time_ms,");
    TEST_ASSERT_NOT_NULL(p);
    p = strchr(p, '\n') + 1;
    long last = LONG_MIN, first_sample = LONG_MIN, last_sample = LONG_MIN, gap = 0;
    int samples = 0, out_of_order = 0;
    bool launch = false, apogee = false, landing = false;
    for (const char *eol; (eol = strchr(p, '\n')) != NULL; p = eol + 1) {
        long t = strtol(p, NULL, 10);
        const char *event = eol;
        while (event > p && event[-1] != ',')
            event--;
        size_t ev_len = (size_t)(eol - event);
        out_of_order += t < last;
        last = t;
        if (ev_len == 0) {
            if (last_sample != LONG_MIN && t - last_sample > gap)
                gap = t - last_sample;
            if (first_sample == LONG_MIN)
                first_sample = t;
            last_sample = t;
            samples++;
        }
        launch |= ev_len == 6 && strncmp(event, "LAUNCH", 6) == 0;
        apogee |= ev_len == 6 && strncmp(event, "APOGEE", 6) == 0;
        landing |= ev_len == 7 && strncmp(event, "LANDING", 7) == 0;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%d sample rows from %ld to %ld ms, the widest gap %ld ms", samples, first_sample,
             last_sample, gap);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out_of_order, msg);
    TEST_ASSERT_TRUE_MESSAGE(launch && apogee && landing, msg);
    TEST_ASSERT_TRUE_MESSAGE(gap > 0 && gap <= 20, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE((last_sample - first_sample) / 20 + 1, samples, msg);
}

/* [DAT-08] A log replayed through the firmware decides the same flight. */
void test_DAT_08_a_log_replays_to_the_same_events(void) {
    flight_conditions_t c = {
        .config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=40\n", .rate_ms = {12.0f, 5.0f}, .to_landed = true};
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

static int with_crlf_line_ends(const char *lf, char *crlf, int size) {
    int n = 0;
    for (; *lf && n < size - 2; lf++) {
        if (*lf == '\n')
            crlf[n++] = '\r';
        crlf[n++] = *lf;
    }
    crlf[n] = '\0';
    return n;
}

/* [DAT-08] A log saved through a tool that writes CRLF line ends. */
void test_DAT_08_a_crlf_log_replays_as_its_lf_twin(void) {
    flight_conditions_t c = {
        .config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=40\n", .rate_ms = {12.0f, 5.0f}, .to_landed = true};
    (void)fly(&ROCKETS[HOP].r, &ISA, &c, 22, 120.0f);
    static char lf[4 * 1024 * 1024], crlf[5 * 1024 * 1024];
    TEST_ASSERT_TRUE(test_flight_log_csv(lf, (int)sizeof(lf)) > 0);
    with_crlf_line_ends(lf, crlf, (int)sizeof(crlf));
    replay_events_t from_lf, from_crlf;
    boot_like_hardware(22);
    TEST_ASSERT_TRUE(replay_run(lf, &from_lf));
    boot_like_hardware(22);
    TEST_ASSERT_TRUE_MESSAGE(replay_run(crlf, &from_crlf), "a CRLF log was refused");
    TEST_ASSERT_EQUAL_INT(from_lf.rows, from_crlf.rows);
    TEST_ASSERT_EQUAL_UINT32(from_lf.apogee_ms, from_crlf.apogee_ms);
    TEST_ASSERT_EQUAL_UINT32(from_lf.pyro2_ms, from_crlf.pyro2_ms);
    TEST_ASSERT_EQUAL_UINT32(from_lf.landing_ms, from_crlf.landing_ms);
}

/* [DAT-08] A log whose header says it kept a row a second is refused, rather
 * than decided from a fiftieth of the readings. */
void test_DAT_08_a_thinned_log_is_refused(void) {
    flight_conditions_t c = {.rate_ms = {20.0f, 0.0f}, .to_landed = true};
    (void)fly(subsonic(), &ISA, &c, 21, 300.0f);
    static char csv[4 * 1024 * 1024], thinned[4 * 1024 * 1024];
    TEST_ASSERT_TRUE(test_flight_log_csv(csv, (int)sizeof(csv)) > 0);
    const char *rate = strstr(csv, "# Log rate: every sample\n");
    TEST_ASSERT_NOT_NULL(rate);
    snprintf(thinned, sizeof(thinned), "%.*s# Log rate: 1 row/s\n%s", (int)(rate - csv), csv,
             rate + strlen("# Log rate: every sample\n"));
    replay_events_t decided;
    boot_like_hardware(21);
    TEST_ASSERT_TRUE_MESSAGE(replay_run(csv, &decided), "the log as written replays");
    boot_like_hardware(21);
    TEST_ASSERT_FALSE(replay_run(thinned, &decided));
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

/* The slowest rocket designed for: 5:1 off the pad, 11.5 m/s at the end of
 * a 1.7 m rail [FLT-LAUNCH-08]. */
static const mp_rocket_t five_to_one = {2.5f, 0.5f, 123.0f, 3.0f, 0.004f, 1.0f, 20.0f};

/* Seconds from T+0 to the LAUNCH row. */
static float seen_leaving_s(const mp_rocket_t *rocket, float sensor_rms_pa) {
    flight_conditions_t c = {.sensor_rms_pa = sensor_rms_pa, .stop_after_apogee = true};
    flown_t r = fly(rocket, &ISA, &c, 29, 120.0f);
    TEST_ASSERT_TRUE(r.launched);
    return harness_log_event_time("LAUNCH");
}

void test_FLT_LAUNCH_08_the_slowest_rocket_is_seen_leaving_within_1_s(void) {
    const float noises[] = {SENSOR_RMS_PA, NOISY_SENSOR_RMS_PA};
    for (unsigned n = 0; n < 2; n++) {
        float slowest = seen_leaving_s(&five_to_one, noises[n]);
        float faster = seen_leaving_s(subsonic(), noises[n]);
        char msg[96];
        snprintf(msg, sizeof(msg), "%.1f Pa: 5:1 seen %.2f s after T+0, subsonic %.2f s", (double)noises[n],
                 (double)slowest, (double)faster);
        TEST_ASSERT_TRUE_MESSAGE(slowest >= 0.0f && slowest <= 1.0f, msg);
        TEST_ASSERT_TRUE_MESSAGE(faster >= 0.0f && faster < slowest, msg);
    }
}

/* [DAT-11] The log opens on the pad: rows before T+0, at the ground, for a
 * brisk rocket and for the slowest designed for. */
static void starts_on_the_ground(const mp_rocket_t *rocket, const char *what) {
    flight_conditions_t c = {.stop_after_apogee = true};
    flown_t r = fly(rocket, &ISA, &c, 29, 120.0f);
    TEST_ASSERT_TRUE_MESSAGE(r.launched, what);
    static char csv[1024 * 1024];
    TEST_ASSERT_TRUE(test_flight_log_csv(csv, (int)sizeof(csv)) > 0);
    const char *row = strchr(strstr(csv, "time_ms,"), '\n') + 1;
    long t = 0, alt_cm = 0, state = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, sscanf(row, "%ld,%*ld,%ld,%ld", &t, &alt_cm, &state), what);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: launch declared %.2f s in, the first row at %ld ms, %ld cm, state %ld", what,
             (double)r.launch_t, t, alt_cm, state);
    TEST_ASSERT_TRUE_MESSAGE(t <= -500, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(PAD_IDLE, state, msg);
    TEST_ASSERT_TRUE_MESSAGE(labs(alt_cm) < 200, msg);
}

void test_DAT_11_the_log_starts_on_the_ground(void) {
    starts_on_the_ground(subsonic(), "subsonic");
    starts_on_the_ground(&five_to_one, "5:1 thrust to weight");
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
    RUN_TEST(test_FLT_APO_01_another_boards_separation_at_the_top_does_not_delay_apogee);
    RUN_TEST(test_FLT_APO_12_apogee_waits_for_the_users_lockout);
    RUN_TEST(test_SNS_MAX_01_above_the_ceiling_apogee_is_never_early);
    RUN_TEST(test_SNS_MAX_01_best_effort_above_the_sensors_range);
    RUN_TEST(test_SNS_ALT_01_the_reported_peak_is_not_clamped);
    RUN_TEST(test_SNS_ALT_01_below_the_pad_is_a_negative_altitude);
    RUN_TEST(test_TST_06_a_canopy_slows_the_descent);
    RUN_TEST(test_FLT_DESC_02_a_ballistic_flight_reaches_landed);
    RUN_TEST(test_FLT_DESC_01_the_phase_follows_the_rate_not_the_channel);
    RUN_TEST(test_FLT_LAND_02_a_slow_descent_is_not_a_landing);
    RUN_TEST(test_DAT_04_the_events_of_a_flight_in_order);
    RUN_TEST(test_FLT_LAUNCH_03_time_zero_is_the_start_of_the_rise);
    RUN_TEST(test_FLT_LOG_07_every_sample_is_a_row);
    RUN_TEST(test_FLT_LAUNCH_08_the_slowest_rocket_is_seen_leaving_within_1_s);
    RUN_TEST(test_DAT_11_the_log_starts_on_the_ground);
    RUN_TEST(test_DAT_08_a_log_replays_to_the_same_events);
    RUN_TEST(test_DAT_08_a_crlf_log_replays_as_its_lf_twin);
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
