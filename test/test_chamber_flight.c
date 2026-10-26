/*
 * A pressure chamber run by hand, flown through the flight software
 * [FLT-APO-08..11, PYR-MODE-05]: the recorded raw readings in test_data/. Its
 * climb and its fall are faster than any flight, so the model filter never
 * explains them; the simple filter must still find the top, and the flight
 * must still land.
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

#define RECORDING "test_data/pressure_chamber.csv"
#define POINTS_MAX 400

typedef struct {
    float t_s, pa;
} point_t;

static point_t recorded[POINTS_MAX];
static int points;
static float pad_pa, top_s, top_pa;

static void load_recording(void) {
    FILE *f = fopen(RECORDING, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open " RECORDING);
    char line[96];
    points = 0;
    while (fgets(line, sizeof(line), f) && points < POINTS_MAX) {
        float t, pa;
        if (line[0] == '#' || sscanf(line, "%f,%f", &t, &pa) != 2)
            continue;
        recorded[points++] = (point_t){t, pa};
    }
    fclose(f);
    TEST_ASSERT_TRUE(points > 100);
    pad_pa = recorded[0].pa;
    top_pa = recorded[0].pa;
    for (int i = 0; i < points; i++) {
        if (recorded[i].pa <= top_pa) {
            top_pa = recorded[i].pa;
            top_s = recorded[i].t_s;
        }
    }
}

static float recorded_pa(float t_s) {
    if (t_s <= 0.0f)
        return pad_pa;
    for (int i = 1; i < points; i++) {
        if (t_s < recorded[i].t_s) {
            float span = recorded[i].t_s - recorded[i - 1].t_s;
            float along = (t_s - recorded[i - 1].t_s) / span;
            return recorded[i - 1].pa + along * (recorded[i].pa - recorded[i - 1].pa);
        }
    }
    return pad_pa;
}

static struct {
    float apogee_s, landed_s;
    uint32_t start_ms;
} seen;

static bool logged(const char *row) {
    for (int i = 0; i < mock_estimator_row_count; i++)
        if (strncmp(mock_estimator_rows[i], row, strlen(row)) == 0)
            return true;
    return false;
}

static float height_of(float pa) {
    return atmos_height_above_m(pa, pad_pa);
}

static void fly_the_chamber(bool model_decides) {
    load_recording();
    boot_like_hardware(5);
    mock_pressure.pressure_pa = pad_pa;
    char ini[128];
    snprintf(ini, sizeof(ini), "[pyro]\nunits=m\nmodel_decides=%s\n", model_decides ? "true" : "false");
    harness_config(ini);
    seen.apogee_s = seen.landed_s = -1.0f;

    uint32_t t = 0;
    uint32_t start = run_to_pad(&t) + 12000u;
    seen.start_ms = start;
    for (; t < start + 150000u; t++) {
        float flight_s = ((float)t - (float)start) / 1000.0f;
        mock_pressure.pressure_pa = recorded_pa(flight_s);
        tick(t);
        if (ctx.apogee_declared && seen.apogee_s < 0.0f)
            seen.apogee_s = flight_s;
        if (ctx.current_state == LANDED && seen.landed_s < 0.0f)
            seen.landed_s = flight_s;
    }
}

void test_FLT_APO_09_with_the_simple_filter_deciding_the_chamber_flight_has_its_apogee(void) {
    fly_the_chamber(false);
    TEST_ASSERT_TRUE_MESSAGE(seen.apogee_s > 0.0f, "declared");
    TEST_ASSERT_TRUE_MESSAGE(seen.apogee_s >= top_s, "not before the lowest reading");
    TEST_ASSERT_TRUE(seen.apogee_s < top_s + 1.5f);
    TEST_ASSERT_TRUE_MESSAGE(ctx.apogee_by_simple, "by the simple filter");
    TEST_ASSERT_TRUE_MESSAGE(seen.landed_s > 0.0f, "and it lands");
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("APOGEE"));
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("LANDING"));
}

void test_FLT_APO_10_with_the_model_deciding_the_chamber_flight_still_has_its_apogee(void) {
    fly_the_chamber(true);
    TEST_ASSERT_TRUE_MESSAGE(seen.apogee_s > 0.0f, "declared");
    TEST_ASSERT_TRUE_MESSAGE(seen.apogee_s >= top_s, "not before the lowest reading");
    TEST_ASSERT_TRUE_MESSAGE(seen.landed_s > 0.0f, "and it lands");
}

void test_FLT_APO_08_the_peak_is_the_simple_filters_top(void) {
    fly_the_chamber(false);
    TEST_ASSERT_FALSE(ctx.peak_lower_bound);
    TEST_ASSERT_FLOAT_WITHIN(20.0f, height_of(top_pa), (float)ctx.max_altitude_cm / 100.0f);
}

void test_PYR_MODE_05_delay_counts_from_the_simple_filters_top(void) {
    fly_the_chamber(false);
    float apogee_time_s = ((float)ctx.apogee_time - (float)seen.start_ms) / 1000.0f;
    TEST_ASSERT_TRUE_MESSAGE(apogee_time_s >= top_s, "never placed early");
    TEST_ASSERT_TRUE_MESSAGE(apogee_time_s < seen.apogee_s, "before the declaration");
    TEST_ASSERT_TRUE(apogee_time_s < top_s + 0.3f);
}

void test_FLT_APO_11_the_log_says_which_filter_declared_apogee(void) {
    fly_the_chamber(false);
    TEST_ASSERT_TRUE(logged("apogee by simple"));
    TEST_ASSERT_TRUE(logged("simple APOGEE"));
    TEST_ASSERT_TRUE(logged("simple cm="));
    fly_the_chamber(true);
    TEST_ASSERT_TRUE(logged("apogee by simple"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_APO_09_with_the_simple_filter_deciding_the_chamber_flight_has_its_apogee);
    RUN_TEST(test_FLT_APO_10_with_the_model_deciding_the_chamber_flight_still_has_its_apogee);
    RUN_TEST(test_FLT_APO_08_the_peak_is_the_simple_filters_top);
    RUN_TEST(test_PYR_MODE_05_delay_counts_from_the_simple_filters_top);
    RUN_TEST(test_FLT_APO_11_the_log_says_which_filter_declared_apogee);
    return UNITY_END();
}
