/*
 * The filtered state every decision is made on [SNS-EST-01..05, DD-085]:
 * the estimator through its own interface, and an AGL trigger flown from
 * pads at three elevations.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include "board_harness.h"
#include "flight_run.h"
#include "../src/atmosphere.h"
#include "../src/pressure_estimator.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f
#define STEP_US 20000u

/* ── A sensor: the truth, plus Gaussian noise, in whole pascals ───── */

static uint32_t rng;

static float uniform(void) {
    rng = rng * 1664525u + 1013904223u;
    return ((float)(rng >> 8) + 0.5f) / 16777216.0f;
}

static float gaussian(void) {
    return sqrtf(-2.0f * logf(uniform())) * cosf(6.2831853f * uniform());
}

static int32_t reading(float true_pa, float noise_pa) {
    return (int32_t)(true_pa + noise_pa * gaussian());
}

typedef float (*height_fn)(float t);

static pest_t filter;
static uint32_t t_us;

static void start_filter(uint32_t seed, float pa, float noise_pa) {
    rng = seed;
    t_us = 0;
    pest_start(&filter, reading(pa, noise_pa), t_us, PEST_NOISE_FLOOR_PA);
}

static void feed(float true_pa, float noise_pa) {
    t_us += STEP_US;
    pest_update(&filter, reading(true_pa, noise_pa), t_us);
}

static void follow(height_fn height, float from_s, float to_s, float noise_pa) {
    for (float t = from_s; t < to_s; t += 0.02f)
        feed(atmos_pressure_above_pa(PAD_PA, height(t)), noise_pa);
}

static float speed_ms(void) {
    pest_estimate_t e = pest_estimate(&filter);
    return -e.rate * atmos_scale_height_m(e.pressure_pa);
}

static float height_m(void) {
    return atmos_height_above_m(pest_estimate(&filter).pressure_pa, PAD_PA);
}

/* ── It follows the flight [SNS-EST-01] ───────────────────────────── */

static float coast(float t) { /* thrown up at 150 m/s from 500 m */
    return 500.0f + 150.0f * t - 4.9f * t * t;
}

void test_SNS_EST_01_the_state_is_the_pressure_its_rate_and_its_acceleration(void) {
    const float noises[] = {SENSOR_RMS_PA, NOISY_SENSOR_RMS_PA};
    for (unsigned n = 0; n < 2; n++) {
        start_filter(1 + n, atmos_pressure_above_pa(PAD_PA, coast(0.0f)), noises[n]);
        float worst_h = 0.0f, worst_v = 0.0f;
        for (float t = 0.0f; t < 30.0f; t += 0.02f) {
            feed(atmos_pressure_above_pa(PAD_PA, coast(t + 0.02f)), noises[n]);
            if (t < 3.0f)
                continue;
            worst_h = fmaxf(worst_h, fabsf(height_m() - coast(t + 0.02f)));
            worst_v = fmaxf(worst_v, fabsf(speed_ms() - (150.0f - 9.8f * (t + 0.02f))));
        }
        TEST_ASSERT_TRUE_MESSAGE(worst_h < 1.5f, "the height, through the whole coast and the fall");
        TEST_ASSERT_TRUE_MESSAGE(worst_v < 2.5f, "the speed");
        pest_estimate_t e = pest_estimate(&filter);
        float accel = -e.curve * atmos_scale_height_m(e.pressure_pa);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4.0f, -9.8f, accel, "and the acceleration");
    }
}

/* The noise is measured, not assumed, and never taken below the quietest
 * sensor fitted. */
void test_SNS_EST_01_the_sensor_noise_is_tracked(void) {
    const float noises[] = {1.2f, 2.5f, NOISY_SENSOR_RMS_PA, 20.0f};
    for (unsigned n = 0; n < 4; n++) {
        start_filter(10 + n, PAD_PA, noises[n]);
        for (int i = 0; i < 1500; i++)
            feed(PAD_PA, noises[n]);
        TEST_ASSERT_FLOAT_WITHIN(0.35f * noises[n], noises[n], pest_estimate(&filter).noise_pa);
    }
    start_filter(20, PAD_PA, 0.0f);
    for (int i = 0; i < 1500; i++)
        feed(PAD_PA, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, PEST_NOISE_FLOOR_PA, pest_estimate(&filter).noise_pa);
}

/* ── Bad readings and real changes [SNS-EST-02] ───────────────────── */

void test_SNS_EST_02_one_or_two_bad_readings_leave_the_state_alone(void) {
    const float off[] = {-60000.0f, -3000.0f, -300.0f, 300.0f, 20000.0f};
    for (int run = 1; run <= 2; run++) {
        for (unsigned k = 0; k < 5; k++) {
            start_filter(30 + k, PAD_PA, 1.2f);
            for (int i = 0; i < 500; i++)
                feed(PAD_PA, 1.2f);
            for (int i = 0; i < run; i++)
                feed(PAD_PA + off[k], 1.2f);
            float worst_v = 0.0f, worst_pa = 0.0f;
            for (int i = 0; i < 250; i++) {
                feed(PAD_PA, 1.2f);
                worst_v = fmaxf(worst_v, fabsf(speed_ms()));
                worst_pa = fmaxf(worst_pa, fabsf(pest_estimate(&filter).pressure_pa - PAD_PA));
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "%d reading(s) %.0f Pa off", run, (double)off[k]);
            TEST_ASSERT_TRUE_MESSAGE(worst_v < 1.0f, msg);
            TEST_ASSERT_TRUE_MESSAGE(worst_pa < 4.0f, msg);
        }
    }
}

/* A step that stays is data: the state goes to it. */
void test_SNS_EST_02_a_sustained_run_is_followed(void) {
    const float steps[] = {-3000.0f, 800.0f, -120.0f};
    for (unsigned k = 0; k < 3; k++) {
        start_filter(40 + k, PAD_PA, 1.2f);
        for (int i = 0; i < 500; i++)
            feed(PAD_PA, 1.2f);
        for (int i = 0; i < 250; i++)
            feed(PAD_PA + steps[k], 1.2f);
        TEST_ASSERT_FLOAT_WITHIN(0.05f * fabsf(steps[k]) + 4.0f, PAD_PA + steps[k], pest_estimate(&filter).pressure_pa);
    }
}

/* ── Noise crosses nothing [SNS-EST-03] ───────────────────────────── */

static float under_canopy_24(float t) {
    return 3000.0f - 24.0f * t;
}

/* Steady at 24 m/s, 20 % under a 30 m/s rule: never reported past it, on a
 * quiet sensor or a noisy one. */
void test_SNS_EST_03_a_steady_descent_below_a_rule_never_reads_past_it(void) {
    const float noises[] = {SENSOR_RMS_PA, NOISY_SENSOR_RMS_PA};
    for (unsigned n = 0; n < 2; n++) {
        start_filter(50 + n, atmos_pressure_above_pa(PAD_PA, under_canopy_24(0.0f)), noises[n]);
        float fastest = 0.0f;
        for (float t = 0.0f; t < 110.0f; t += 0.02f) {
            feed(atmos_pressure_above_pa(PAD_PA, under_canopy_24(t + 0.02f)), noises[n]);
            if (t > 3.0f)
                fastest = fmaxf(fastest, -speed_ms());
        }
        TEST_ASSERT_TRUE_MESSAGE(fastest < 30.0f, "noise alone crossed the rule");
        TEST_ASSERT_TRUE(fastest > 23.0f);
    }
}

void test_SNS_EST_03_a_still_board_reads_still(void) {
    start_filter(60, PAD_PA, NOISY_SENSOR_RMS_PA);
    float fastest = 0.0f;
    for (int i = 0; i < 30000; i++) { /* ten minutes */
        feed(PAD_PA, NOISY_SENSOR_RMS_PA);
        if (i > 150)
            fastest = fmaxf(fastest, fabsf(speed_ms()));
    }
    pest_estimate_t e = pest_estimate(&filter);
    float sigma_ms = e.rate_sigma * atmos_scale_height_m(e.pressure_pa);
    TEST_ASSERT_TRUE_MESSAGE(sigma_ms < 1.0f, "the speed is known to under 1 m/s on the noisiest sensor");
    TEST_ASSERT_TRUE_MESSAGE(fastest < 4.0f * sigma_ms, "and its stated uncertainty is honest");
}

/* ── A real change is followed [SNS-EST-04] ───────────────────────── */

/* Under a canopy at 20 m/s until it is lost at t = 20 s, then free fall. */
static float canopy_lost(float t) {
    if (t < 20.0f)
        return 3000.0f - 20.0f * t;
    float dt = t - 20.0f;
    return 2600.0f - 20.0f * dt - 4.9f * dt * dt;
}

void test_SNS_EST_04_a_speed_that_passes_a_rule_is_reported_within_two_seconds(void) {
    const float noises[] = {SENSOR_RMS_PA, NOISY_SENSOR_RMS_PA};
    for (unsigned n = 0; n < 2; n++) {
        start_filter(70 + n, atmos_pressure_above_pa(PAD_PA, canopy_lost(0.0f)), noises[n]);
        follow(canopy_lost, 0.02f, 20.0f, noises[n]);
        /* The truth passes 35 m/s at 20 + 15/9.8 = 21.53 s. */
        float seen_at = 0.0f;
        for (float t = 20.0f; t < 30.0f && seen_at == 0.0f; t += 0.02f) {
            feed(atmos_pressure_above_pa(PAD_PA, canopy_lost(t)), noises[n]);
            if (-speed_ms() > 35.0f)
                seen_at = t;
        }
        TEST_ASSERT_TRUE_MESSAGE(seen_at >= 21.4f, "not before the truth passed it");
        TEST_ASSERT_TRUE_MESSAGE(seen_at < 21.53f + 2.0f, "and within two seconds of it");
    }
}

/* ── Heights are converted once, against the pad [SNS-EST-05] ─────── */

void test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad(void) {
    /* The standard atmosphere's own air at each elevation. */
    const mp_site_t pads[] = {{15.0f, 0.0f}, {8.5f, 1000.0f}, {2.0f, 2000.0f}};
    const uint16_t heights[] = {150, 300, 600};
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned h = 0; h < 3; h++) {
            flight_conditions_t c = {.main_m = heights[h], .rate_ms = {8.0f, 5.0f}};
            flown_t f = fly(&ROCKETS[SUBSONIC].r, &pads[p], &c, 80 + 3 * p + h, 400.0f);
            char msg[64];
            snprintf(msg, sizeof(msg), "%u m from a pad at %.0f m: fired at %.1f m", (unsigned)heights[h],
                     (double)pads[p].elev_m, (double)f.main_h);
            TEST_ASSERT_TRUE_MESSAGE(f.main, msg);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f * (float)heights[h], (float)heights[h], f.main_h, msg);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_EST_01_the_state_is_the_pressure_its_rate_and_its_acceleration);
    RUN_TEST(test_SNS_EST_01_the_sensor_noise_is_tracked);
    RUN_TEST(test_SNS_EST_02_one_or_two_bad_readings_leave_the_state_alone);
    RUN_TEST(test_SNS_EST_02_a_sustained_run_is_followed);
    RUN_TEST(test_SNS_EST_03_a_steady_descent_below_a_rule_never_reads_past_it);
    RUN_TEST(test_SNS_EST_03_a_still_board_reads_still);
    RUN_TEST(test_SNS_EST_04_a_speed_that_passes_a_rule_is_reported_within_two_seconds);
    RUN_TEST(test_SNS_EST_05_an_agl_trigger_acts_at_its_height_from_any_pad);
    return UNITY_END();
}
