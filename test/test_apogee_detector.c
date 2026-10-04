/*
 * The apogee rule alone [FLT-APO-07]: seen climbing, then seen falling, with
 * the estimator explaining the readings throughout.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/apogee_detector.h"

void setUp(void) {}
void tearDown(void) {}

#define SIGMA 0.0001f
#define STEP_MS 20u

static const estimate_t CLIMBING = {.rate = -10.0f * SIGMA, .rate_sigma = SIGMA, .explains = true};
static const estimate_t FALLING = {.rate = +10.0f * SIGMA, .rate_sigma = SIGMA, .explains = true};
static const estimate_t NEAR_THE_TOP = {.rate = +2.0f * SIGMA, .rate_sigma = SIGMA, .explains = true};
static const estimate_t NOT_EXPLAINED = {.rate = +10.0f * SIGMA, .rate_sigma = SIGMA, .explains = false};

/* Feeds the same estimate for a time; true if apogee was detected in it. */
static bool fed(apogee_detector_t *d, const estimate_t *e, bool believed, uint32_t *ms, uint32_t for_ms) {
    bool detected = false;
    for (uint32_t end = *ms + for_ms; *ms < end; *ms += STEP_MS)
        detected |= apogee_detected(d, e, believed, *ms);
    return detected;
}

void test_FLT_APO_07_over_the_top_is_apogee_at_once(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE_MESSAGE(fed(&d, &NEAR_THE_TOP, true, &ms, 1000), "within its own uncertainty it is not falling");
    TEST_ASSERT_TRUE(apogee_detected(&d, &FALLING, true, ms));
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
    TEST_ASSERT_FALSE_MESSAGE(apogee_detected(&d, &FALLING, true, ms), "the climb before is no longer evidence");
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1980));
    TEST_ASSERT_TRUE(fed(&d, &FALLING, true, &ms, 60));
}

void test_FLT_APO_07_a_suspect_reading_decides_nothing_and_forgets_nothing(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &CLIMBING, true, &ms, 3000));
    TEST_ASSERT_FALSE(fed(&d, &FALLING, false, &ms, 200));
    TEST_ASSERT_TRUE_MESSAGE(apogee_detected(&d, &FALLING, true, ms), "the climb is still seen");
}

void test_FLT_APO_07_a_fall_interrupted_starts_its_2_s_again(void) {
    apogee_detector_t d = {0};
    uint32_t ms = 1000;
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1500));
    TEST_ASSERT_FALSE(fed(&d, &NEAR_THE_TOP, true, &ms, 100));
    TEST_ASSERT_FALSE(fed(&d, &FALLING, true, &ms, 1500));
    TEST_ASSERT_TRUE(fed(&d, &FALLING, true, &ms, 600));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_APO_07_over_the_top_is_apogee_at_once);
    RUN_TEST(test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last_2_s);
    RUN_TEST(test_FLT_APO_07_readings_not_explained_forget_the_climb);
    RUN_TEST(test_FLT_APO_07_a_suspect_reading_decides_nothing_and_forgets_nothing);
    RUN_TEST(test_FLT_APO_07_a_fall_interrupted_starts_its_2_s_again);
    return UNITY_END();
}
