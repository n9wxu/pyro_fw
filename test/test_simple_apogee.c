/*
 * The simple filter alone [FLT-APO-09, FLT-APO-10]: a median of three raw
 * readings standing 2 m below its lowest point for 0.5 s, and the speed
 * lockout beside it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/simple_apogee.h"
#include "../src/atmosphere.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f
#define STEP_MS 20u
#define G 9.80665f

static int32_t at_height(float h_m) {
    return (int32_t)(atmos_pressure_above_pa(PAD_PA, h_m) + 0.5f);
}

/* A coast over the top: up at v0 from h0, under gravity alone. */
static float coast_m(float h0, float v0, float t_s) {
    return h0 + v0 * t_s - 0.5f * G * t_s * t_s;
}

/* Feeds a coast from t0 for a time; returns the first time the rule was met,
 * or 0. */
static uint32_t fly(simple_apogee_t *f, float h0, float v0, uint32_t *ms, uint32_t for_ms) {
    uint32_t met = 0;
    uint32_t start = *ms;
    for (uint32_t end = *ms + for_ms; *ms < end; *ms += STEP_MS) {
        simple_apogee_reading(f, at_height(coast_m(h0, v0, (float)(*ms - start) / 1000.0f)), *ms);
        if (!met && simple_apogee_met(f, *ms))
            met = *ms;
    }
    return met;
}

void test_FLT_APO_09_a_coast_over_the_top_is_apogee_once_the_rise_has_held(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    float v0 = 40.0f;
    uint32_t met = fly(&f, 100.0f, v0, &ms, 10000);
    uint32_t top_ms = 1000 + (uint32_t)(v0 / G * 1000.0f);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, met, "declared");
    /* 2 m below the top is 0.64 s down; then held 0.5 s. */
    TEST_ASSERT_TRUE_MESSAGE(met >= top_ms + 1100, "not before the fall has held");
    TEST_ASSERT_TRUE(met <= top_ms + 1300);
    TEST_ASSERT_TRUE_MESSAGE(simple_apogee_lowest_ms(&f) >= top_ms, "the top is never placed early");
    TEST_ASSERT_TRUE(simple_apogee_lowest_ms(&f) <= top_ms + 150);
}

void test_FLT_APO_09_nothing_is_declared_below_30_m(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fly(&f, 0.0f, 20.0f, &ms, 4000), "a 20 m hop and back");
}

void test_FLT_APO_09_a_single_reading_off_is_not_seen(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    for (int i = 0; i < 50; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(500.0f), ms);
    simple_apogee_reading(&f, at_height(500.0f) + 100000, ms);
    ms += STEP_MS;
    for (int i = 0; i < 50; i++, ms += STEP_MS) {
        simple_apogee_reading(&f, at_height(500.0f), ms);
        TEST_ASSERT_FALSE(simple_apogee_met(&f, ms));
    }
}

void test_FLT_APO_09_an_ejection_spike_shorter_than_the_hold_is_not_apogee(void) {
    /* 400 ms of readings 30 kPa high, still climbing: eight times the longest
     * separation shock. */
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    uint32_t start = ms;
    for (; ms < start + 3000; ms += STEP_MS) {
        float h = coast_m(100.0f, 40.0f, (float)(ms - start) / 1000.0f);
        bool spike = ms >= start + 1000 && ms < start + 1400;
        simple_apogee_reading(&f, at_height(h) + (spike ? 30000 : 0), ms);
        TEST_ASSERT_FALSE_MESSAGE(simple_apogee_met(&f, ms), "still climbing");
    }
}

void test_FLT_APO_09_a_new_lowest_point_starts_the_hold_again(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    for (int i = 0; i < 50; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(500.0f), ms);
    for (int i = 0; i < 20; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(495.0f), ms);
    TEST_ASSERT_FALSE_MESSAGE(simple_apogee_met(&f, ms), "5 m down for 0.4 s");
    for (int i = 0; i < 20; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(510.0f), ms);
    for (int i = 0; i < 20; i++, ms += STEP_MS) {
        simple_apogee_reading(&f, at_height(505.0f), ms);
        TEST_ASSERT_FALSE_MESSAGE(simple_apogee_met(&f, ms), "the climb to 510 m restarted it");
    }
    for (int i = 0; i < 10; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(505.0f), ms);
    TEST_ASSERT_TRUE(simple_apogee_met(&f, ms));
}

void test_FLT_APO_09_readings_of_zero_or_less_are_not_readings(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    for (int i = 0; i < 50; i++, ms += STEP_MS)
        simple_apogee_reading(&f, at_height(500.0f), ms);
    for (int i = 0; i < 50; i++, ms += STEP_MS)
        simple_apogee_reading(&f, 0, ms);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 500.0f, simple_apogee_height_m(&f));
    TEST_ASSERT_FALSE(simple_apogee_met(&f, ms));
}

void test_FLT_APO_10_the_lockout_holds_while_the_speed_is_over_50_m_s(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    uint32_t start = ms;
    for (; ms < start + 5000; ms += STEP_MS) {
        simple_apogee_reading(&f, at_height(100.0f + 80.0f * (float)(ms - start) / 1000.0f), ms);
        TEST_ASSERT_FALSE(simple_apogee_released(&f));
    }
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 80.0f, simple_apogee_speed_ms(&f));
}

void test_FLT_APO_10_the_lockout_releases_after_3_s_under_50_m_s_and_closes_on_a_fast_fall(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    uint32_t start = ms;
    uint32_t released = 0;
    for (; ms < start + 6000; ms += STEP_MS) {
        simple_apogee_reading(&f, at_height(500.0f + 40.0f * (float)(ms - start) / 1000.0f), ms);
        if (!released && simple_apogee_released(&f))
            released = ms;
    }
    TEST_ASSERT_NOT_EQUAL(0, released);
    TEST_ASSERT_UINT32_WITHIN_MESSAGE(250, start + 3500, released, "3 s once a speed is measured");
    uint32_t back = ms;
    for (; ms < back + 1000; ms += STEP_MS)
        simple_apogee_reading(&f, at_height(740.0f - 200.0f * (float)(ms - back) / 1000.0f), ms);
    TEST_ASSERT_FALSE_MESSAGE(simple_apogee_released(&f), "a fall at 200 m/s locks it again");
}

void test_FLT_APO_10_a_slow_spell_cut_short_does_not_release(void) {
    simple_apogee_t f;
    simple_apogee_start(&f, PAD_PA);
    uint32_t ms = 1000;
    float h = 500.0f;
    for (int spell = 0; spell < 4; spell++) {
        for (int i = 0; i < 100; i++, ms += STEP_MS) /* 2 s at 20 m/s */
            simple_apogee_reading(&f, at_height(h += 0.4f), ms);
        for (int i = 0; i < 25; i++, ms += STEP_MS) /* 0.5 s at 100 m/s */
            simple_apogee_reading(&f, at_height(h += 2.0f), ms);
        TEST_ASSERT_FALSE(simple_apogee_released(&f));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_APO_09_a_coast_over_the_top_is_apogee_once_the_rise_has_held);
    RUN_TEST(test_FLT_APO_09_nothing_is_declared_below_30_m);
    RUN_TEST(test_FLT_APO_09_a_single_reading_off_is_not_seen);
    RUN_TEST(test_FLT_APO_09_an_ejection_spike_shorter_than_the_hold_is_not_apogee);
    RUN_TEST(test_FLT_APO_09_a_new_lowest_point_starts_the_hold_again);
    RUN_TEST(test_FLT_APO_09_readings_of_zero_or_less_are_not_readings);
    RUN_TEST(test_FLT_APO_10_the_lockout_holds_while_the_speed_is_over_50_m_s);
    RUN_TEST(test_FLT_APO_10_the_lockout_releases_after_3_s_under_50_m_s_and_closes_on_a_fast_fall);
    RUN_TEST(test_FLT_APO_10_a_slow_spell_cut_short_does_not_release);
    return UNITY_END();
}
