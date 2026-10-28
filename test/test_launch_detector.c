/*
 * The launch trigger alone [FLT-LAUNCH-02, FLT-LAUNCH-07]: 15 m above the
 * pad on the filtered state, whatever its speed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/launch_detector.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* The filtered state the trigger reads: a height above the pad and a climb
 * speed. */
static bool launch_at(float height_m, float climb_ms) {
    pp_sample_t s = {0};
    s.height_m = height_m;
    s.speed = climb_ms;
    return launch_detected(&s);
}

void test_FLT_LAUNCH_07_high_and_climbing_is_a_launch(void) {
    TEST_ASSERT_TRUE(launch_at(40.0f, 20.0f));
    TEST_ASSERT_TRUE(launch_at(3000.0f, 300.0f));
}

void test_FLT_LAUNCH_02_below_15_m_is_not_a_launch(void) {
    TEST_ASSERT_FALSE(launch_at(0.0f, 200.0f));
    TEST_ASSERT_FALSE(launch_at(14.5f, 200.0f));
    TEST_ASSERT_TRUE_MESSAGE(launch_at(15.5f, 200.0f), "and just above it is");
}

/* The pad speed swings in gusts; the height alone decides. */
void test_FLT_LAUNCH_07_the_speed_plays_no_part(void) {
    TEST_ASSERT_TRUE(launch_at(20.0f, 1.0f));
    TEST_ASSERT_TRUE(launch_at(20.0f, -5.0f));
    TEST_ASSERT_FALSE(launch_at(10.0f, 100.0f));
}

void test_FLT_LAUNCH_07_no_estimate_is_no_launch(void) {
    pp_sample_t s = {0};
    TEST_ASSERT_FALSE(launch_detected(&s));
    TEST_ASSERT_FALSE_MESSAGE(launch_at(NAN, 0.0f), "nor a height that is not a number");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LAUNCH_07_high_and_climbing_is_a_launch);
    RUN_TEST(test_FLT_LAUNCH_02_below_15_m_is_not_a_launch);
    RUN_TEST(test_FLT_LAUNCH_07_the_speed_plays_no_part);
    RUN_TEST(test_FLT_LAUNCH_07_no_estimate_is_no_launch);
    return UNITY_END();
}
