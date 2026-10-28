/*
 * The standard atmosphere the readings are converted with, against the 1976
 * standard's published values, and the pad's air the heights are in
 * [SNS-EST-05, FLT-AIR-01, DD-114].
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

void test_FLT_AIR_01_temperature_at_each_altitude(void) {
    for (int i = 0; i < N_STANDARD; i++)
        TEST_ASSERT_FLOAT_WITHIN(0.1f, STANDARD[i].k, atmos_temperature_at_k(STANDARD[i].m));
}

/* R T / (g p): 8.32 cm a pascal at sea level, 53 cm at 15 km. */
void test_SNS_EST_05_metres_per_pascal_follow_the_air(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.08324f, atmos_m_per_pa(0.0f, 101325.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5265f, atmos_m_per_pa(15000.0f, 12044.6f));
}

/* In the standard's own air a height above the pad is the difference of two
 * altitudes, so a high pad is as right as a sea-level one. */
void test_SNS_EST_05_a_height_is_measured_from_the_pad(void) {
    const float pads_m[] = {0.0f, 500.0f, 1500.0f, 2000.0f};
    const float heights_m[] = {30.48f, 150.0f, 300.0f, 1000.0f, 9000.0f, 25000.0f};
    for (int i = 0; i < 4; i++) {
        air_t air = air_over_pad(atmos_pressure_pa(pads_m[i]), 0.0f);
        for (int j = 0; j < 6; j++) {
            float pa = atmos_pressure_pa(pads_m[i] + heights_m[j]);
            float h = air_height_m(&air, atmos_altitude_m(pa));
            TEST_ASSERT_FLOAT_WITHIN(0.5f + 0.001f * heights_m[j], heights_m[j], h);
        }
    }
}

/* [SNS-ALT-01] Below the pad is a negative height. */
void test_SNS_ALT_01_below_the_pad_is_negative(void) {
    air_t air = air_over_pad(atmos_pressure_pa(1000.0f), 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -50.0f, air_height_m(&air, 950.0f));
}

/* [DD-114] Air 15 K warmer than the standard's is that much thinner with
 * height: the same pressures are 5 % higher. A pad with no temperature is
 * the standard's. */
void test_SNS_EST_05_a_warm_pads_air_stands_taller(void) {
    air_t warm = air_over_pad(101325.0f, 288.15f + 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 15.0f, warm.offset_k);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 300.0f * (303.15f / 288.15f), air_height_m(&warm, 300.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f + 15.0f / 288.15f, air_scale(&warm, 0.0f));
    air_t none = air_over_pad(101325.0f, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, none.offset_k);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, air_height_m(&none, 300.0f));
}

/* The altitude at a height, for the bench's own flights, inverts the height. */
void test_SNS_EST_05_a_height_inverts_to_its_altitude(void) {
    const float pad_k[] = {0.0f, 263.15f, 318.15f};
    const float heights_m[] = {-20.0f, 15.0f, 300.0f, 3000.0f, 12000.0f, 25000.0f};
    for (int i = 0; i < 3; i++) {
        air_t air = air_over_pad(atmos_pressure_pa(1500.0f), pad_k[i]);
        for (int j = 0; j < 6; j++)
            TEST_ASSERT_FLOAT_WITHIN(0.05f, heights_m[j], air_height_m(&air, air_altitude_m(&air, heights_m[j])));
    }
}

/* A canopy's terminal rate goes as 1/sqrt(rho): the ratio is 1 at the pad and
 * falls with the air. */
void test_FLT_AIR_01_pad_air_ratio(void) {
    air_t sea = air_over_pad(101325.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, air_pad_ratio(&sea, 0.0f));
    float rho_30km = 1171.87f / (287.05287f * 226.65f), rho_0 = 1.2250f;
    TEST_ASSERT_FLOAT_WITHIN(0.002f, sqrtf(rho_30km / rho_0), air_pad_ratio(&sea, 30000.0f));
    air_t warm_high = air_over_pad(atmos_pressure_pa(1500.0f), 303.15f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, air_pad_ratio(&warm_high, 1500.0f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SNS_EST_05_pressure_at_the_standards_altitudes);
    RUN_TEST(test_SNS_EST_05_altitude_inverts_pressure);
    RUN_TEST(test_FLT_AIR_01_temperature_at_each_altitude);
    RUN_TEST(test_SNS_EST_05_metres_per_pascal_follow_the_air);
    RUN_TEST(test_SNS_EST_05_a_height_is_measured_from_the_pad);
    RUN_TEST(test_SNS_ALT_01_below_the_pad_is_negative);
    RUN_TEST(test_SNS_EST_05_a_warm_pads_air_stands_taller);
    RUN_TEST(test_SNS_EST_05_a_height_inverts_to_its_altitude);
    RUN_TEST(test_FLT_AIR_01_pad_air_ratio);
    return UNITY_END();
}
