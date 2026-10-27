/*
 * The apogee rule alone [FLT-APO-07]: seen climbing, then seen falling, with
 * the estimator explaining the readings and the readings close to the
 * filtered height throughout.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/apogee_detector.h"
#include "../src/atmosphere.h"

void setUp(void) {}
void tearDown(void) {}

#define PA 90000.0f
#define SIGMA 0.0001f
#define STEP_MS 20u

#define AT(r, explained)                                                                                               \
    { .pressure_pa = PA, .rate = (r) * SIGMA, .rate_sigma = SIGMA, .noise_pa = 2.0f, .explains = explained }
static const estimate_t CLIMBING = AT(-10.0f, true);
static const estimate_t FALLING = AT(+10.0f, true);
static const estimate_t NEAR_THE_TOP = AT(+1.5f, true);
static const estimate_t NOT_EXPLAINED = AT(+10.0f, false);

/* A reading this many metres above the filtered height. */
static int32_t reading_off_by(const estimate_t *e, float m) {
    return (int32_t)atmos_pressure_above_pa(e->pressure_pa, m);
}

/* Feeds the same estimate for a time, its readings off by off_m; true if
 * apogee was detected in it. */
static bool fed_off(apogee_detector_t *d, const estimate_t *e, float off_m, bool believed, uint32_t *ms,
                    uint32_t for_ms) {
    bool detected = false;
    for (uint32_t end = *ms + for_ms; *ms < end; *ms += STEP_MS)
        detected |= apogee_detected(d, e, reading_off_by(e, off_m), believed, *ms);
    return detected;
}

static bool fed(apogee_detector_t *d, const estimate_t *e, bool believed, uint32_t *ms, uint32_t for_ms) {
    return fed_off(d, e, 0.0f, believed, ms, for_ms);
}

static bool now(apogee_detector_t *d, const estimate_t *e, uint32_t ms) {
    return apogee_detected(d, e, reading_off_by(e, 0.0f), true, ms);
}

void test_FLT_APO_07_over_the_top_is_apogee_at_once(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE_MESSAGE(fed(&d, &NEAR_THE_TOP, true, &ms, 1000), "within its own uncertainty it is not falling");
    TEST_ASSERT_TRUE(now(&d, &FALLING, ms));
}

void test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last_2_s(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1980));
    TEST_ASSERT_TRUE(fed(&d, &FALLING, true, &ms, 40));
}

void test_FLT_APO_07_readings_not_explained_forget_the_climb(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE(fed(&d, &NOT_EXPLAINED, true, &ms, 5000));
    TEST_ASSERT_FALSE_MESSAGE(now(&d, &FALLING, ms), "the climb before is no longer evidence");
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1980));
    TEST_ASSERT_TRUE(fed(&d, &FALLING, true, &ms, 60));
}

void test_FLT_APO_07_a_suspect_reading_decides_nothing_and_forgets_nothing(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE(fed(&d, &FALLING, false, &ms, 200));
    TEST_ASSERT_TRUE_MESSAGE(now(&d, &FALLING, ms), "the climb is still seen");
}

void test_FLT_APO_07_a_fall_interrupted_starts_its_2_s_again(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1500));
    TEST_ASSERT_FALSE(fed(&d, &NEAR_THE_TOP, true, &ms, 100));
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1500));
    TEST_ASSERT_TRUE(fed(&d, &FALLING, true, &ms, 600));
}

/* A port error near Mach 1 throws the readings off the filtered height, and
 * builds on the way up. */
void test_FLT_APO_07_readings_far_from_the_filter_hold_apogee_until_they_settle(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE(fed_off(&d, &CLIMBING, 30.0f, true, &ms, 500));
    TEST_ASSERT_FALSE(fed_off(&d, &FALLING, 30.0f, true, &ms, 1000));
    TEST_ASSERT_FALSE_MESSAGE(fed_off(&d, &FALLING, -30.0f, true, &ms, 1000), "in either direction");
    TEST_ASSERT_TRUE_MESSAGE(fed(&d, &FALLING, true, &ms, 1000), "the error decays reading by reading");
}

/* [SNS-EST-02] A separation's shock, ours or another board's: readings far
 * off for under 50 ms are no error of the filter's. */
void test_FLT_APO_07_a_shock_holds_nothing(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE(fed_off(&d, &NEAR_THE_TOP, -450.0f, true, &ms, 40));
    TEST_ASSERT_TRUE(now(&d, &FALLING, ms));
}

void test_FLT_APO_07_readings_within_10_m_of_the_filter_are_tracking(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed_off(&d, &CLIMBING, 8.0f, true, &ms, 3000));
    TEST_ASSERT_TRUE(fed_off(&d, &FALLING, 8.0f, true, &ms, STEP_MS));
}

/* Where the air is thin the sensor's own noise is tens of metres. */
void test_FLT_APO_07_in_thin_air_the_bound_allows_for_the_sensors_noise(void) {
    estimate_t climbing = CLIMBING, falling = FALLING;
    climbing.pressure_pa = falling.pressure_pa = 1200.0f;
    climbing.noise_pa = falling.noise_pa = 9.0f;
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed_off(&d, &climbing, 60.0f, true, &ms, 3000));
    TEST_ASSERT_TRUE(fed_off(&d, &falling, 60.0f, true, &ms, STEP_MS));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_APO_07_over_the_top_is_apogee_at_once);
    RUN_TEST(test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last_2_s);
    RUN_TEST(test_FLT_APO_07_readings_not_explained_forget_the_climb);
    RUN_TEST(test_FLT_APO_07_a_suspect_reading_decides_nothing_and_forgets_nothing);
    RUN_TEST(test_FLT_APO_07_a_fall_interrupted_starts_its_2_s_again);
    RUN_TEST(test_FLT_APO_07_readings_far_from_the_filter_hold_apogee_until_they_settle);
    RUN_TEST(test_FLT_APO_07_a_shock_holds_nothing);
    RUN_TEST(test_FLT_APO_07_readings_within_10_m_of_the_filter_are_tracking);
    RUN_TEST(test_FLT_APO_07_in_thin_air_the_bound_allows_for_the_sensors_noise);
    return UNITY_END();
}
