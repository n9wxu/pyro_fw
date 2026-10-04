/*
 * The launch trigger alone [FLT-LAUNCH-02, FLT-LAUNCH-07]: 100 ft above the
 * pad and climbing at 5 m/s or more, on the filtered state.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/atmosphere.h"
#include "../src/launch_detector.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f

/* The filtered state the trigger reads: a height above the pad and a climb
 * speed, as the estimator reports them. */
static bool launch_at(float height_m, float climb_ms) {
    pp_sample_t s = {0};
    s.pressure_pa = atmos_pressure_above_pa(PAD_PA, height_m);
    s.rate = -climb_ms / atmos_scale_height_m(s.pressure_pa);
    return launch_detected(&s, PAD_PA);
}

void test_FLT_LAUNCH_07_high_and_climbing_is_a_launch(void) {
    TEST_ASSERT_TRUE(launch_at(40.0f, 20.0f));
    TEST_ASSERT_TRUE(launch_at(3000.0f, 300.0f));
}

void test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch(void) {
    TEST_ASSERT_FALSE(launch_at(0.0f, 200.0f));
    TEST_ASSERT_FALSE(launch_at(30.0f, 200.0f));
    TEST_ASSERT_TRUE_MESSAGE(launch_at(31.0f, 200.0f), "and just above it is");
}

void test_FLT_LAUNCH_07_slower_than_5_m_s_is_not_a_launch(void) {
    TEST_ASSERT_FALSE(launch_at(500.0f, 4.9f));
    TEST_ASSERT_FALSE(launch_at(500.0f, 0.0f));
    TEST_ASSERT_FALSE_MESSAGE(launch_at(500.0f, -30.0f), "nor is a descent");
    TEST_ASSERT_TRUE(launch_at(500.0f, 5.1f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LAUNCH_07_high_and_climbing_is_a_launch);
    RUN_TEST(test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch);
    RUN_TEST(test_FLT_LAUNCH_07_slower_than_5_m_s_is_not_a_launch);
    return UNITY_END();
}
