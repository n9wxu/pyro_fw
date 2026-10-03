/*
 * Each estimator the build carries, through the interface alone
 * [SNS-EST-06, SNS-EST-08, SNS-EST-09]: the same readings to every one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/atmosphere.h"
#include "../src/estimator.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f
#define STEP_US 20000u
#define G 9.80665f

static const estimator_vt *e;
static uint32_t t_us;
static uint32_t noise_state;

/* About 1.5 Pa rms, the same every run. */
static int32_t noise_pa(void) {
    noise_state = noise_state * 1664525u + 1013904223u;
    return (int32_t)((noise_state >> 24) % 7u) - 3;
}

static void on_the_pad(uint8_t estimator) {
    e = estimator_at(estimator);
    t_us = 5000000u;
    noise_state = 1u;
    e->start((int32_t)PAD_PA, t_us, 288.15f);
}

static estimate_t read_for(float pressure_pa, uint32_t for_us) {
    for (uint32_t end = t_us + for_us; t_us != end;) {
        t_us += STEP_US;
        e->reading((int32_t)pressure_pa + noise_pa(), t_us);
    }
    estimate_t out;
    e->estimate(&out);
    return out;
}

static estimate_t nothing_for(uint32_t for_us) {
    for (uint32_t end = t_us + for_us; t_us != end;) {
        t_us += STEP_US;
        e->no_reading(t_us);
    }
    estimate_t out;
    e->estimate(&out);
    return out;
}

/* A burn at 5 g for 2 s and a coast in no air: the height after t seconds. */
#define BURN_S 2.0f
#define BURN_MS2 (5.0f * G)
static float arc_height_m(float t) {
    if (t <= BURN_S)
        return 0.5f * BURN_MS2 * t * t;
    float coast = t - BURN_S;
    return 0.5f * BURN_MS2 * BURN_S * BURN_S + BURN_MS2 * BURN_S * coast - 0.5f * G * coast * coast;
}
#define ARC_APOGEE_S (BURN_S + BURN_MS2 * BURN_S / G)

static estimate_t fly_the_arc_to(float until_s) {
    static float flown_s;
    if (until_s <= 0.0f)
        flown_s = 0.0f;
    estimate_t out;
    e->estimate(&out);
    while (flown_s < until_s) {
        flown_s += (float)STEP_US * 1e-6f;
        out = read_for(atmos_pressure_above_pa(PAD_PA, arc_height_m(flown_s)), STEP_US);
    }
    return out;
}

#define EVERY_ESTIMATOR for (uint8_t i = 0; i < estimator_count(); i++)

void test_SNS_EST_06_each_has_a_name_config_can_hold(void) {
    EVERY_ESTIMATOR {
        const estimator_vt *a = estimator_at(i);
        TEST_ASSERT_TRUE_MESSAGE(strlen(a->name) >= 1 && strlen(a->name) <= 8, a->name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(i, estimator_index(a->name), a->name);
        TEST_ASSERT_TRUE_MESSAGE(a->state_size <= ESTIMATOR_STATE_MAX, a->name);
    }
    TEST_ASSERT_TRUE(estimator_count() <= ESTIMATORS_MAX);
}

void test_SNS_EST_08_explaining_takes_a_second_of_readings(void) {
    EVERY_ESTIMATOR {
        on_the_pad(i);
        TEST_ASSERT_FALSE_MESSAGE(read_for(PAD_PA, 500000u).explains, e->name);
        estimate_t later = read_for(PAD_PA, 1500000u);
        TEST_ASSERT_TRUE_MESSAGE(later.explains, e->name);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, PAD_PA, later.pressure_pa, e->name);
        TEST_ASSERT_TRUE_MESSAGE(fabsf(later.rate) <= 4.0f * later.rate_sigma, e->name);
    }
}

/* 2 kPa in one reading, and staying: 170 m in 20 ms is no rocket's motion. */
void test_SNS_EST_08_a_step_no_motion_makes_is_not_explained(void) {
    EVERY_ESTIMATOR {
        on_the_pad(i);
        TEST_ASSERT_TRUE_MESSAGE(read_for(PAD_PA, 5000000u).explains, e->name);
        bool explained_throughout = true;
        for (int k = 0; k < 25; k++)
            explained_throughout &= read_for(PAD_PA - 2000.0f, STEP_US).explains;
        TEST_ASSERT_FALSE_MESSAGE(explained_throughout, e->name);
        TEST_ASSERT_TRUE_MESSAGE(read_for(PAD_PA - 2000.0f, 10000000u).explains, "and readings that stay are followed");
    }
}

void test_SNS_EST_08_a_few_missing_readings_are_not_a_gap(void) {
    EVERY_ESTIMATOR {
        on_the_pad(i);
        TEST_ASSERT_TRUE_MESSAGE(read_for(PAD_PA, 5000000u).explains, e->name);
        TEST_ASSERT_TRUE_MESSAGE(nothing_for(160000u).explains, e->name);
        TEST_ASSERT_TRUE_MESSAGE(read_for(PAD_PA, 100000u).explains, e->name);
        TEST_ASSERT_FALSE_MESSAGE(nothing_for(400000u).explains, e->name);
        TEST_ASSERT_FALSE_MESSAGE(read_for(PAD_PA, 500000u).explains, e->name);
        TEST_ASSERT_TRUE_MESSAGE(read_for(PAD_PA, 1500000u).explains, e->name);
    }
}

/* The same arc to every estimator: climbing before the top, falling after
 * it, and explained either side. */
void test_SNS_EST_08_a_ballistic_arc_is_explained_over_the_top(void) {
    EVERY_ESTIMATOR {
        on_the_pad(i);
        (void)read_for(PAD_PA, 5000000u);
        (void)fly_the_arc_to(0.0f);
        estimate_t before = fly_the_arc_to(ARC_APOGEE_S - 2.0f);
        estimate_t after = fly_the_arc_to(ARC_APOGEE_S + 2.0f);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: 2 s before the top %.1f m/s (explains %d), 2 s after %.1f m/s (explains %d)",
                 e->name, (double)(-before.rate * atmos_scale_height_m(before.pressure_pa)), before.explains,
                 (double)(-after.rate * atmos_scale_height_m(after.pressure_pa)), after.explains);
        TEST_ASSERT_TRUE_MESSAGE(before.explains && after.explains, msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, 2.0f * G, -before.rate * atmos_scale_height_m(before.pressure_pa), msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(3.0f, -2.0f * G, -after.rate * atmos_scale_height_m(after.pressure_pa), msg);
    }
}

/* A minute with no readings, climbing at 49 m/s when they stopped. An
 * estimate may carry on over the top by its own model, and it does not fall
 * through the ground: it is never more than a metre below the pad. It takes
 * the readings up again when they come back. */
void test_SNS_EST_08_an_estimate_does_not_fall_through_the_ground(void) {
    EVERY_ESTIMATOR {
        on_the_pad(i);
        (void)read_for(PAD_PA, 5000000u);
        (void)fly_the_arc_to(0.0f);
        (void)fly_the_arc_to(ARC_APOGEE_S - 5.0f);
        float lowest_m = 1e9f;
        estimate_t after = {0};
        for (int second = 0; second < 60; second++) {
            after = nothing_for(1000000u);
            lowest_m = fminf(lowest_m, atmos_height_above_m(after.pressure_pa, PAD_PA));
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: as low as %.0f m with no readings", e->name, (double)lowest_m);
        TEST_ASSERT_FALSE_MESSAGE(after.explains, msg);
        TEST_ASSERT_TRUE_MESSAGE(lowest_m > -1.0f, msg);
        estimate_t back = read_for(PAD_PA, 10000000u);
        snprintf(msg, sizeof(msg), "%s: %.0f Pa from the readings 10 s after they returned", e->name,
                 (double)(back.pressure_pa - PAD_PA));
        TEST_ASSERT_TRUE_MESSAGE(back.explains, msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(20.0f, PAD_PA, back.pressure_pa, msg);
    }
}

/* [SNS-EST-09] A charge pressurises the bay by 3 kPa, dying away in 0.2 s.
 * Told of the pulse, the lumped estimator does not take the bay for the air. */
void test_SNS_EST_09_the_lumped_estimator_sits_out_its_own_charge(void) {
    on_the_pad(estimator_index("lumped"));
    TEST_ASSERT_EQUAL_STRING("lumped", e->name);
    (void)read_for(PAD_PA, 5000000u);
    e->pulse(t_us);
    float furthest_pa = 0.0f;
    for (int k = 0; k < 50; k++) {
        float bay_pa = PAD_PA + 3000.0f * expf(-(float)k * 0.02f / 0.2f);
        estimate_t now = read_for(bay_pa, STEP_US);
        furthest_pa = fmaxf(furthest_pa, fabsf(now.pressure_pa - PAD_PA));
        TEST_ASSERT_FALSE_MESSAGE(now.explains && k > 15, "nothing is decided inside the charge");
    }
    TEST_ASSERT_TRUE_MESSAGE(furthest_pa < 30.0f, "the estimate followed the bay");
    TEST_ASSERT_TRUE(read_for(PAD_PA, 2000000u).explains);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_EST_06_each_has_a_name_config_can_hold);
    RUN_TEST(test_SNS_EST_08_explaining_takes_a_second_of_readings);
    RUN_TEST(test_SNS_EST_08_a_step_no_motion_makes_is_not_explained);
    RUN_TEST(test_SNS_EST_08_a_few_missing_readings_are_not_a_gap);
    RUN_TEST(test_SNS_EST_08_a_ballistic_arc_is_explained_over_the_top);
    RUN_TEST(test_SNS_EST_08_an_estimate_does_not_fall_through_the_ground);
    RUN_TEST(test_SNS_EST_09_the_lumped_estimator_sits_out_its_own_charge);
    return UNITY_END();
}
