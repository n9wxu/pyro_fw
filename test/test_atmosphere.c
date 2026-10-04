/*
 * The standard atmosphere the flight comparisons are converted with
 * [SNS-EST-05, FLT-AIR-01], against the 1976 standard's published values.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/atmosphere.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* U.S. Standard Atmosphere, 1976, Table 4: geopotential altitude, K, Pa. */
static const struct {
    float m, k, pa;
} STANDARD[] = {
    {0.0f, 288.15f, 101325.0f},  {1000.0f, 281.65f, 89874.6f}, {5000.0f, 255.65f, 54019.9f},
    {11000.0f, 216.65f, 22632.1f}, {15000.0f, 216.65f, 12044.6f}, {20000.0f, 216.65f, 5474.89f},
    {25000.0f, 221.65f, 2511.02f}, {30000.0f, 226.65f, 1171.87f}, {32000.0f, 228.65f, 868.019f},
    {40000.0f, 251.05f, 277.522f},
};
#define N_STANDARD (int)(sizeof(STANDARD) / sizeof(STANDARD[0]))

void test_SNS_EST_05_pressure_at_the_standards_altitudes(void) {
    for (int i = 0; i < N_STANDARD; i++)
        TEST_ASSERT_FLOAT_WITHIN(0.001f * STANDARD[i].pa, STANDARD[i].pa, atmos_pressure_pa(STANDARD[i].m));
}

void test_SNS_EST_05_altitude_inverts_pressure(void) {
    for (int i = 0; i < N_STANDARD; i++)
        TEST_ASSERT_FLOAT_WITHIN(2.0f + 0.0005f * STANDARD[i].m, STANDARD[i].m, atmos_altitude_m(STANDARD[i].pa));
}

void test_FLT_AIR_01_temperature_at_each_pressure(void) {
    for (int i = 0; i < N_STANDARD; i++)
        TEST_ASSERT_FLOAT_WITHIN(0.1f, STANDARD[i].k, atmos_temperature_k(STANDARD[i].pa));
}

/* A height above the pad is the difference of two standard altitudes, so a
 * high pad is as right as a sea-level one. */
void test_SNS_EST_05_a_height_is_measured_from_the_pad(void) {
    const float pads_m[] = {0.0f, 500.0f, 1500.0f, 2000.0f};
    const float heights_m[] = {30.48f, 150.0f, 300.0f, 1000.0f, 9000.0f, 25000.0f};
    for (int i = 0; i < 4; i++) {
        float pad_pa = atmos_pressure_pa(pads_m[i]);
        for (int j = 0; j < 6; j++) {
            float pa = atmos_pressure_above_pa(pad_pa, heights_m[j]);
            TEST_ASSERT_FLOAT_WITHIN(0.5f + 0.001f * heights_m[j], heights_m[j], atmos_height_above_m(pa, pad_pa));
            TEST_ASSERT_FLOAT_WITHIN(0.001f * pa, atmos_pressure_pa(pads_m[i] + heights_m[j]), pa);
        }
    }
}

/* [SNS-ALT-01] Below the pad is a negative height. */
void test_SNS_ALT_01_below_the_pad_is_negative(void) {
    float pad_pa = atmos_pressure_pa(1000.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -50.0f, atmos_height_above_m(atmos_pressure_pa(950.0f), pad_pa));
}

void test_FLT_AIR_01_scale_height_follows_the_temperature(void) {
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 8434.5f, atmos_scale_height_m(101325.0f));
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 6341.6f, atmos_scale_height_m(12044.6f));
}

/* A canopy's terminal rate goes as 1/sqrt(rho): the ratio is 1 at the pad and
 * falls with the air. */
void test_FLT_AIR_01_pad_air_ratio(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, atmos_pad_air_ratio(101325.0f, 101325.0f));
    float rho_30km = 1171.87f / (287.05287f * 226.65f), rho_0 = 1.2250f;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, sqrtf(rho_30km / rho_0), atmos_pad_air_ratio(1171.87f, 101325.0f));
    float high_pad = atmos_pressure_pa(1500.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, atmos_pad_air_ratio(high_pad, high_pad));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_EST_05_pressure_at_the_standards_altitudes);
    RUN_TEST(test_SNS_EST_05_altitude_inverts_pressure);
    RUN_TEST(test_FLT_AIR_01_temperature_at_each_pressure);
    RUN_TEST(test_SNS_EST_05_a_height_is_measured_from_the_pad);
    RUN_TEST(test_SNS_ALT_01_below_the_pad_is_negative);
    RUN_TEST(test_FLT_AIR_01_scale_height_follows_the_temperature);
    RUN_TEST(test_FLT_AIR_01_pad_air_ratio);
    return UNITY_END();
}
