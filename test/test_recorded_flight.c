/*
 * A recorded trajectory flown through the flight software [TST-02]: the
 * OpenRocket export in test_data/, a 165 ft hop under one canopy, as the
 * pressure the standard atmosphere puts at each recorded height.
 *
 * Verifies [SYS-DEPLOY-01, SYS-DEPLOY-02, FLT-PHASE-01..03, FLT-ASC-01,
 * FLT-APO-02, FLT-APO-03, SYS-ALT-01, SYS-DATA-01, SYS-TEL-01].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <stdio.h>
#include <string.h>
#include "board_harness.h"
#include "../src/atmosphere.h"

void setUp(void) {}
void tearDown(void) {}

#define RECORDING "test_data/open_rocket_export.csv"
#define PAD_PA 101325.0f
#define FT 0.3048f
#define MAIN_M 30

typedef struct {
    float t_s, h_m;
} point_t;

static point_t recorded[300];
static int points;
static float apogee_s, apogee_m, ground_hit_s;

static void load_recording(void) {
    FILE *f = fopen(RECORDING, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open " RECORDING);
    char line[128];
    points = 0;
    apogee_m = 0.0f;
    while (fgets(line, sizeof(line), f) && points < 300) {
        float t, feet;
        if (line[0] == '#' || sscanf(line, "%f,%f", &t, &feet) != 2)
            continue;
        recorded[points++] = (point_t){t, feet * FT};
        if (feet * FT > apogee_m) {
            apogee_m = feet * FT;
            apogee_s = t;
        }
    }
    fclose(f);
    TEST_ASSERT_TRUE(points > 100);
    ground_hit_s = recorded[points - 1].t_s;
}

static float recorded_height_m(float t_s) {
    if (t_s <= recorded[0].t_s)
        return recorded[0].h_m;
    for (int i = 1; i < points; i++) {
        if (t_s < recorded[i].t_s) {
            float span = recorded[i].t_s - recorded[i - 1].t_s;
            float along = span > 0.0f ? (t_s - recorded[i - 1].t_s) / span : 0.0f;
            return recorded[i - 1].h_m + along * (recorded[i].h_m - recorded[i - 1].h_m);
        }
    }
    return recorded[points - 1].h_m;
}

/* What the flight software did, against the recording's own clock. */
static struct {
    float launch_s, apogee_s, landed_s;
    float pulse_s[2], pulse_h_m[2];
    int pulses[2];
    bool states[STATE_COUNT];
} seen;

static void fly_the_recording(void) {
    load_recording();
    boot_like_hardware(7);
    char ini[96];
    snprintf(ini, sizeof(ini), "[pyro]\nunits=m\npyro1_mode=delay\npyro1_value=0\npyro2_mode=agl\npyro2_value=%d\n",
             MAIN_M);
    harness_config(ini);
    memset(&seen, 0, sizeof(seen));
    seen.launch_s = seen.apogee_s = seen.landed_s = -1.0f;

    uint32_t t = 0;
    uint32_t ignition = run_to_pad(&t) + 12000u;
    int counted = 0;
    for (; t < ignition + (uint32_t)((ground_hit_s + 20.0f) * 1000.0f); t++) {
        float flight_s = ((float)t - (float)ignition) / 1000.0f;
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, recorded_height_m(flight_s));
        tick(t);
        seen.states[ctx.current_state] = true;
        if (ctx.current_state == ASCENT && seen.launch_s < 0.0f)
            seen.launch_s = flight_s;
        if (ctx.apogee_declared && seen.apogee_s < 0.0f)
            seen.apogee_s = flight_s;
        if (ctx.current_state == LANDED && seen.landed_s < 0.0f)
            seen.landed_s = flight_s;
        for (; counted < mock_pulse_count; counted++) {
            int ch = mock_pulses[counted].channel - 1;
            if (seen.pulses[ch]++ == 0) {
                seen.pulse_s[ch] = flight_s;
                seen.pulse_h_m[ch] = recorded_height_m(flight_s);
            }
        }
    }
}

void test_TST_02_the_recorded_flight_runs_pad_to_landed(void) {
    fly_the_recording();
    TEST_ASSERT_TRUE_MESSAGE(seen.states[PAD_IDLE] && seen.states[ASCENT] && seen.states[LANDED],
                             "the pad, the ascent and the landing [FLT-PHASE-01, FLT-PHASE-03]");
    TEST_ASSERT_TRUE_MESSAGE(seen.states[FALLING] || seen.states[DROGUE_DESCENT] || seen.states[CHUTE_DESCENT],
                             "a descent [FLT-APO-02]");
    TEST_ASSERT_EQUAL(LANDED, ctx.current_state);
    const char *order[] = {"LAUNCH", "APOGEE", "PYRO1", "PYRO2", "LANDING"};
    float last = -1.0f;
    for (unsigned i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_log_events(order[i]), order[i]);
        TEST_ASSERT_TRUE_MESSAGE(harness_log_event_time(order[i]) >= last, order[i]);
        last = harness_log_event_time(order[i]);
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, "$PYRO,"), "and it was sent down [SYS-TEL-01]");
}

void test_TST_02_launch_is_detected_as_the_rocket_leaves(void) {
    fly_the_recording();
    TEST_ASSERT_TRUE_MESSAGE(seen.launch_s > 0.0f, "not before ignition");
    TEST_ASSERT_TRUE_MESSAGE(recorded_height_m(seen.launch_s) >= 30.0f, "above 100 ft [FLT-LAUNCH-02]");
    TEST_ASSERT_TRUE(recorded_height_m(seen.launch_s) < 45.0f);
}

void test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon(void) {
    fly_the_recording();
    TEST_ASSERT_TRUE_MESSAGE(seen.apogee_s >= apogee_s, "never early [FLT-APO-01]");
    TEST_ASSERT_TRUE(seen.apogee_s < apogee_s + 1.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f * apogee_m + 0.5f, apogee_m, (float)ctx.max_altitude_cm / 100.0f,
                                     "the peak [FLT-ASC-01, SYS-ALT-01]");
}

void test_TST_02_each_channel_fires_once_at_its_event(void) {
    fly_the_recording();
    TEST_ASSERT_EQUAL_INT(1, seen.pulses[0]);
    TEST_ASSERT_EQUAL_INT(1, seen.pulses[1]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, seen.apogee_s, seen.pulse_s[0], "pyro 1 at apogee");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, (float)MAIN_M, seen.pulse_h_m[1], "pyro 2 at its height");
    TEST_ASSERT_TRUE_MESSAGE(seen.pulse_s[1] > seen.pulse_s[0], "in that order");
}

void test_TST_02_landing_follows_the_ground_hit(void) {
    fly_the_recording();
    TEST_ASSERT_TRUE_MESSAGE(seen.landed_s >= ground_hit_s, "not in the air [FLT-LAND-02]");
    TEST_ASSERT_TRUE(seen.landed_s < ground_hit_s + 5.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_altitude_plays, "and the peak is beeped out [SYS-DATA-03]");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_TST_02_the_recorded_flight_runs_pad_to_landed);
    RUN_TEST(test_TST_02_launch_is_detected_as_the_rocket_leaves);
    RUN_TEST(test_TST_02_apogee_is_declared_after_the_recorded_peak_and_soon);
    RUN_TEST(test_TST_02_each_channel_fires_once_at_its_event);
    RUN_TEST(test_TST_02_landing_follows_the_ground_hit);
    return UNITY_END();
}
