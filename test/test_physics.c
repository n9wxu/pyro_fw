/*
 * The simulators' rocket (sim/physics.h): its atmosphere against the
 * U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562, Table I, geopotential
 * altitudes), and the flight it flies.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "physics.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

#define PUBLISHED_TOLERANCE 0.001f /* 0.1 % */

static void assert_within_published(float published, float actual) {
    TEST_ASSERT_FLOAT_WITHIN(published * PUBLISHED_TOLERANCE, published, actual);
}

void test_ussa76_pressure_at_every_layer_base(void) {
    assert_within_published(101325.0f, physics_pressure_pa(0.0f));
    assert_within_published(22632.06f, physics_pressure_pa(11000.0f));
    assert_within_published(5474.889f, physics_pressure_pa(20000.0f));
    assert_within_published(868.0187f, physics_pressure_pa(32000.0f));
    assert_within_published(110.9063f, physics_pressure_pa(47000.0f));
    assert_within_published(66.93887f, physics_pressure_pa(51000.0f));
    assert_within_published(3.956420f, physics_pressure_pa(71000.0f));
}

void test_ussa76_temperature_at_every_layer_base(void) {
    assert_within_published(288.15f, physics_temperature_k(0.0f));
    assert_within_published(216.65f, physics_temperature_k(11000.0f));
    assert_within_published(216.65f, physics_temperature_k(20000.0f));
    assert_within_published(228.65f, physics_temperature_k(32000.0f));
    assert_within_published(270.65f, physics_temperature_k(47000.0f));
    assert_within_published(270.65f, physics_temperature_k(51000.0f));
    assert_within_published(214.65f, physics_temperature_k(71000.0f));
    assert_within_published(186.946f, physics_temperature_k(84852.0f));
}

void test_ussa76_pressure_inside_the_layers(void) {
    assert_within_published(54019.9f, physics_pressure_pa(5000.0f));
    assert_within_published(1171.87f, physics_pressure_pa(30000.0f));
}

void test_ussa76_sea_level_density(void) {
    assert_within_published(1.2250f, physics_density_kg_m3(0.0f));
}

void test_pressure_falls_monotonically_to_100_km(void) {
    float prev = physics_pressure_pa(0.0f);
    for (float h = 100.0f; h <= 100000.0f; h += 100.0f) {
        float p = physics_pressure_pa(h);
        TEST_ASSERT_TRUE(p < prev);
        TEST_ASSERT_TRUE(p > 0.0f);
        prev = p;
    }
}

static float fly_to_apogee(float target_m) {
    physics_state_t ps;
    physics_init(&ps, target_m);
    float t = 0.0f;
    while (ps.vel_ms >= 0.0f && t < 600.0f) {
        physics_step(&ps, t);
        t += PHYS_STEP_S;
    }
    return ps.apogee_m;
}

void test_the_profile_reaches_the_apogee_asked_for(void) {
    const float targets[] = {30.48f, 1524.0f, 30480.0f, 100000.0f};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        TEST_ASSERT_FLOAT_WITHIN(targets[i] * 0.01f, targets[i], fly_to_apogee(targets[i]));
}

void test_the_main_descends_at_its_damping_terminal_speed(void) {
    physics_state_t ps;
    physics_reset(&ps);
    ps.alt_m = 300.0f;
    physics_deploy_main(&ps);
    for (float t = 0.0f; t < 20.0f; t += PHYS_STEP_S)
        physics_step(&ps, 100.0f + t);
    float sigma = physics_density_kg_m3(ps.alt_m) / physics_density_kg_m3(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -PHYS_G0 / (ps.main_damping_per_s * sigma), ps.vel_ms);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ussa76_pressure_at_every_layer_base);
    RUN_TEST(test_ussa76_temperature_at_every_layer_base);
    RUN_TEST(test_ussa76_pressure_inside_the_layers);
    RUN_TEST(test_ussa76_sea_level_density);
    RUN_TEST(test_pressure_falls_monotonically_to_100_km);
    RUN_TEST(test_the_profile_reaches_the_apogee_asked_for);
    RUN_TEST(test_the_main_descends_at_its_damping_terminal_speed);
    return UNITY_END();
}
