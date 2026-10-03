/*
 * The launch trigger alone [FLT-LAUNCH-02, FLT-LAUNCH-07]: 100 ft above the
 * pad, climbing at 5 m/s or more, both held 100 ms, on the filtered state.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/atmosphere.h"
#include "../src/launch_detector.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f
#define STEP_MS 20u

static launch_detector_t detector;
static uint32_t now_ms;

/* The filtered state the trigger reads: a height above the pad and a climb
 * speed, as the estimator reports them. */
static pp_sample_t state(float height_m, float climb_ms) {
    pp_sample_t s = {0};
    s.timestamp_ms = now_ms;
    s.pressure_pa = atmos_pressure_above_pa(PAD_PA, height_m);
    s.rate = -climb_ms / atmos_scale_height_m(s.pressure_pa);
    return s;
}

/* Feeds the same state every sample for ms; true if the trigger fired. */
static bool held(float height_m, float climb_ms, uint32_t ms) {
    bool fired = false;
    for (uint32_t end = now_ms + ms; now_ms < end; now_ms += STEP_MS) {
        pp_sample_t s = state(height_m, climb_ms);
        fired |= launch_detected(&detector, &s, PAD_PA);
    }
    return fired;
}

static void on_the_pad(void) {
    detector = (launch_detector_t){0};
    now_ms = 1000;
}

void test_FLT_LAUNCH_07_high_and_climbing_for_100_ms_is_a_launch(void) {
    on_the_pad();
    TEST_ASSERT_FALSE_MESSAGE(held(40.0f, 20.0f, 80), "not before the 100 ms");
    TEST_ASSERT_TRUE(held(40.0f, 20.0f, 60));
}

void test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch(void) {
    on_the_pad();
    TEST_ASSERT_FALSE(held(30.0f, 200.0f, 2000));
    TEST_ASSERT_TRUE_MESSAGE(held(31.0f, 200.0f, 200), "and just above it is");
}

void test_FLT_LAUNCH_07_slower_than_5_m_s_is_not_a_launch(void) {
    on_the_pad();
    TEST_ASSERT_FALSE(held(500.0f, 4.9f, 2000));
    TEST_ASSERT_FALSE_MESSAGE(held(500.0f, -30.0f, 2000), "nor is a descent");
    TEST_ASSERT_TRUE(held(500.0f, 5.1f, 200));
}

/* The two conditions must hold together and without a break. */
void test_FLT_LAUNCH_07_a_break_in_either_condition_starts_the_100_ms_again(void) {
    on_the_pad();
    TEST_ASSERT_FALSE(held(40.0f, 20.0f, 80));
    TEST_ASSERT_FALSE(held(40.0f, 2.0f, 20));
    TEST_ASSERT_FALSE_MESSAGE(held(40.0f, 20.0f, 80), "the earlier 80 ms do not count");
    TEST_ASSERT_TRUE(held(40.0f, 20.0f, 60));
    on_the_pad();
    TEST_ASSERT_FALSE(held(40.0f, 20.0f, 80));
    TEST_ASSERT_FALSE(held(20.0f, 20.0f, 20));
    TEST_ASSERT_FALSE(held(40.0f, 20.0f, 80));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LAUNCH_07_high_and_climbing_for_100_ms_is_a_launch);
    RUN_TEST(test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch);
    RUN_TEST(test_FLT_LAUNCH_07_slower_than_5_m_s_is_not_a_launch);
    RUN_TEST(test_FLT_LAUNCH_07_a_break_in_either_condition_starts_the_100_ms_again);
    return UNITY_END();
}
