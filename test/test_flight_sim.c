/*
 * The bench flight source (flight_sim.h) [DD-078].
 *
 * The atmosphere against the 1976 US Standard Atmosphere's own tabulated
 * values (NOAA-S/T 76-1562, geopotential altitudes), then the profile, then
 * the bench's hold on the channels (bench_flight.h).
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [SYS-SIM-01, SIM-01..04].
 */
#include "unity.h"
#include "flight_sim.h"
#include "bench_flight.h"
#include <math.h>

void setUp(void) {
    bench_flight_init();
}
void tearDown(void) {}

#define SEA_PA 101325.0f

void test_SIM_02_isa_pressure_at_the_layer_bases(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 101325.0f, fsim_isa_pressure(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 22632.06f, fsim_isa_pressure(11000.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 5474.889f, fsim_isa_pressure(20000.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 868.0187f, fsim_isa_pressure(32000.0f));
}

void test_SIM_02_isa_pressure_inside_the_layers(void) {
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 54019.9f, fsim_isa_pressure(5000.0f));
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 12044.6f, fsim_isa_pressure(15000.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1171.87f, fsim_isa_pressure(30000.0f));
}

void test_SIM_02_isa_density_at_sea_level_and_30_km(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.2250f, fsim_isa_density(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 0.01801f, fsim_isa_density(30000.0f));
}

void test_SIM_02_isa_altitude_inverts_pressure(void) {
    for (float h = 0.0f; h <= 32000.0f; h += 250.0f)
        TEST_ASSERT_FLOAT_WITHIN(0.5f, h, fsim_isa_altitude(fsim_isa_pressure(h)));
}

static fsim_params_t high(void) {
    fsim_params_t p = {.apogee_m = 30000.0f,
                       .boost_s = 4.0f,
                       .drogue_ms = 25.0f,
                       .main_alt_m = 300.0f,
                       .main_ms = 6.0f,
                       .thin_air = true,
                       .pad_s = 5.0f};
    return p;
}

void test_SIM_02_the_coast_peaks_at_the_apogee_asked_for(void) {
    fsim_t s;
    fsim_params_t p = high();
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float peak = 0.0f, t_peak = 0.0f;
    for (float t = 0.0f; t < 120.0f; t += 0.01f) {
        float h = fsim_altitude(&s, t);
        if (h > peak) {
            peak = h;
            t_peak = t;
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 30000.0f, peak);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, p.pad_s + s.t_apogee, t_peak);
}

void test_SIM_02_phases_run_in_order_and_it_lands(void) {
    fsim_t s;
    fsim_params_t p = high();
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    fsim_phase_t last = FSIM_PAD;
    int changes = 0;
    float t = 0.0f;
    for (; t < 3600.0f && fsim_phase(&s) != FSIM_LANDED; t += 0.02f) {
        fsim_altitude(&s, t);
        fsim_phase_t ph = fsim_phase(&s);
        if (ph != last) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(last + 1, ph, fsim_phase_name(ph));
            last = ph;
            changes++;
        }
    }
    TEST_ASSERT_EQUAL_INT(FSIM_LANDED, fsim_phase(&s));
    TEST_ASSERT_EQUAL_INT(5, changes);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fsim_altitude(&s, t + 10.0f));
    TEST_ASSERT_EQUAL_FLOAT(SEA_PA, fsim_pressure(&s, t + 20.0f));
}

/* Without thin air the descent settles at two rates. It leaves apogee at
 * rest, which costs about two seconds against a constant 30 m/s, and carries
 * its speed into the main's height, which gives some of that back. */
void test_SIM_02_descent_times_at_constant_rates(void) {
    fsim_t s;
    fsim_params_t p = {.apogee_m = 3000.0f,
                       .boost_s = 2.0f,
                       .drogue_ms = 30.0f,
                       .main_alt_m = 300.0f,
                       .main_ms = 6.0f,
                       .thin_air = false,
                       .pad_s = 0.0f};
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float t = 0.0f;
    while (fsim_phase(&s) != FSIM_LANDED && t < 1000.0f) {
        fsim_altitude(&s, t);
        t += 0.1f;
    }
    float want = s.t_apogee + 2700.0f / 30.0f + 300.0f / 6.0f;
    TEST_ASSERT_TRUE(s.t_landed > want && s.t_landed < want + 2.5f);
}

/* A parachute at 25 km falls several times faster than at the pad. */
void test_SIM_02_thin_air_speeds_the_drogue(void) {
    fsim_t s;
    fsim_params_t p = high();
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float t = p.pad_s + s.t_apogee;
    while (fsim_altitude(&s, t) > 25000.0f)
        t += 0.1f;
    float h0 = fsim_altitude(&s, t);
    float h1 = fsim_altitude(&s, t + 1.0f);
    float rate_high = h0 - h1;
    float ratio = sqrtf(fsim_isa_density(0.0f) / fsim_isa_density(25000.0f));
    /* Falling into denser air it runs a little ahead of the rate it would
     * settle at. */
    TEST_ASSERT_TRUE(rate_high >= p.drogue_ms * ratio && rate_high < 1.1f * p.drogue_ms * ratio);
    TEST_ASSERT_TRUE(rate_high > 5.0f * p.drogue_ms);
    while (fsim_altitude(&s, t) > 1000.0f)
        t += 0.1f;
    h0 = fsim_altitude(&s, t);
    h1 = fsim_altitude(&s, t + 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, p.drogue_ms, h0 - h1);
}

/* A pad at altitude: the profile is above it, and the pressure is the ISA's
 * at the sum. */
void test_SIM_02_a_high_pad_adds_its_own_altitude(void) {
    fsim_t s;
    fsim_params_t p = high();
    p.apogee_m = 20000.0f;
    float pad_pa = fsim_isa_pressure(1500.0f);
    TEST_ASSERT_TRUE(fsim_start(&s, &p, pad_pa));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1500.0f, s.pad_msl);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, pad_pa, fsim_pressure(&s, 0.0f));
    float t_apo = p.pad_s + s.t_apogee;
    TEST_ASSERT_FLOAT_WITHIN(5.0f, fsim_isa_pressure(21500.0f), fsim_pressure(&s, t_apo));
}

void test_SIM_01_profiles_that_cannot_fly_are_refused(void) {
    fsim_t s;
    fsim_params_t p = high();
    p.apogee_m = 32500.0f;
    TEST_ASSERT_FALSE_MESSAGE(fsim_start(&s, &p, SEA_PA), "above the atmosphere's top");
    p = high();
    p.apogee_m = 31000.0f;
    TEST_ASSERT_FALSE_MESSAGE(fsim_start(&s, &p, fsim_isa_pressure(1500.0f)), "the pad's altitude counts");
    p = high();
    p.main_alt_m = p.apogee_m;
    TEST_ASSERT_FALSE(fsim_start(&s, &p, SEA_PA));
    p = high();
    p.boost_s = 0.0f;
    TEST_ASSERT_FALSE(fsim_start(&s, &p, SEA_PA));
    p = high();
    p.drogue_ms = 0.0f;
    TEST_ASSERT_FALSE(fsim_start(&s, &p, SEA_PA));
    p = high();
    p.main_ms = -1.0f;
    TEST_ASSERT_FALSE(fsim_start(&s, &p, SEA_PA));
    p = high();
    TEST_ASSERT_FALSE(fsim_start(&s, &p, 0.0f));
}

/* [SIM-01] Only from the pad, in test mode, one at a time, and a profile
 * that can fly. */
void test_SIM_01_a_bench_flight_starts_only_from_the_pad_in_test_mode(void) {
    fsim_params_t p = high();
    TEST_ASSERT_EQUAL_INT(BF_NOT_ON_PAD, bench_flight_start(&p, SEA_PA, false, true, 0));
    TEST_ASSERT_EQUAL_INT(BF_NOT_TESTING, bench_flight_start(&p, SEA_PA, true, false, 0));
    p.boost_s = 0.0f;
    TEST_ASSERT_EQUAL_INT(BF_BAD_PROFILE, bench_flight_start(&p, SEA_PA, true, true, 0));
    TEST_ASSERT_FALSE_MESSAGE(bench_flight_mocked(), "a refused start took the channels");
    p = high();
    TEST_ASSERT_EQUAL_INT(BF_STARTED, bench_flight_start(&p, SEA_PA, true, true, 0));
    TEST_ASSERT_EQUAL_INT(BF_RUNNING, bench_flight_start(&p, SEA_PA, true, true, 0));
}

void test_SIM_02_the_bench_replaces_the_reading_while_it_flies(void) {
    float pa = 12345.0f;
    TEST_ASSERT_FALSE(bench_flight_pressure(1000, false, &pa));
    TEST_ASSERT_EQUAL_FLOAT(12345.0f, pa);
    fsim_params_t p = high();
    uint64_t t0 = 10000000u;
    TEST_ASSERT_EQUAL_INT(BF_STARTED, bench_flight_start(&p, SEA_PA, true, true, t0));
    TEST_ASSERT_TRUE(bench_flight_pressure(t0 - 5000u, false, &pa));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SEA_PA, pa, "a reading from before the start is the pad's");
    uint64_t apogee_us = t0 + (uint64_t)((p.pad_s + 4.0f + 0.5f) * 1e6f);
    TEST_ASSERT_TRUE(bench_flight_pressure(apogee_us, false, &pa));
    TEST_ASSERT_TRUE(pa < SEA_PA * 0.9f);
    bench_flight_status_t st;
    bench_flight_status(&st);
    TEST_ASSERT_TRUE(st.flying);
    TEST_ASSERT_EQUAL_INT(FSIM_COAST, st.phase);
}

/* It ends when both have landed: the profile, and the machine. */
void test_SIM_02_the_bench_ends_when_both_have_landed(void) {
    fsim_params_t p = {.apogee_m = 500.0f,
                       .boost_s = 1.0f,
                       .drogue_ms = 50.0f,
                       .main_alt_m = 100.0f,
                       .main_ms = 20.0f,
                       .thin_air = false,
                       .pad_s = 0.0f};
    float pa;
    TEST_ASSERT_EQUAL_INT(BF_STARTED, bench_flight_start(&p, SEA_PA, true, true, 0));
    TEST_ASSERT_TRUE(bench_flight_pressure(5000000u, true, &pa));
    bench_flight_status_t st;
    bench_flight_status(&st);
    TEST_ASSERT_TRUE_MESSAGE(st.flying, "the machine said LANDED in the air, and the profile stopped");
    TEST_ASSERT_TRUE(bench_flight_pressure(300000000u, false, &pa));
    bench_flight_status(&st);
    TEST_ASSERT_EQUAL_INT(FSIM_LANDED, st.phase);
    TEST_ASSERT_TRUE_MESSAGE(st.flying, "the profile landed and stopped before the machine did");
    TEST_ASSERT_TRUE(bench_flight_pressure(301000000u, true, &pa));
    TEST_ASSERT_EQUAL_FLOAT(SEA_PA, pa);
    TEST_ASSERT_FALSE(bench_flight_pressure(302000000u, true, &pa));
}

/* ── Canopies that fail [SIM-04] ──────────────────────────────────── */

static float speed_at(fsim_t *s, float t) {
    return (fsim_altitude(s, t) - fsim_altitude(s, t + 0.5f)) / 0.5f;
}

static fsim_params_t low(void) {
    fsim_params_t p = {.apogee_m = 1500.0f,
                       .boost_s = 2.0f,
                       .drogue_ms = 25.0f,
                       .main_alt_m = 300.0f,
                       .main_ms = 6.0f,
                       .thin_air = false,
                       .pad_s = 0.0f};
    return p;
}

static float time_to(fsim_t *s, float from_t, float height) {
    float t = from_t;
    while (fsim_altitude(s, t) > height && t < 2000.0f)
        t += 0.1f;
    return t;
}

/* The descent leaves apogee at rest and gathers speed under gravity: no
 * flight software sees a speed appear from nowhere. */
void test_SIM_04_the_descent_starts_from_rest(void) {
    fsim_t s;
    fsim_params_t p = low();
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float apogee = s.t_apogee;
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 0.5f * 9.80665f * 1.0f, fsim_altitude(&s, apogee) - fsim_altitude(&s, apogee + 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, p.drogue_ms, speed_at(&s, apogee + 15.0f));
}

void test_SIM_04_a_failed_drogue_falls_ballistic_until_the_main(void) {
    fsim_t s;
    fsim_params_t p = low();
    p.drogue_fails = true;
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float t = time_to(&s, s.t_apogee, 500.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4.0f, FSIM_BALLISTIC_MS, speed_at(&s, t), "ballistic above the main's height");
    t = time_to(&s, t, 150.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, p.main_ms, speed_at(&s, t), "and the main's rate below it");
}

void test_SIM_04_a_failed_main_stays_at_the_drogues_rate(void) {
    fsim_t s;
    fsim_params_t p = low();
    p.main_fails = true;
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float t = time_to(&s, s.t_apogee, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, p.drogue_ms, speed_at(&s, t));
}

void test_SIM_04_with_both_failed_it_falls_ballistic_to_the_ground(void) {
    fsim_t s;
    fsim_params_t p = low();
    p.drogue_fails = p.main_fails = true;
    p.ballistic_ms = 60.0f;
    TEST_ASSERT_TRUE(fsim_start(&s, &p, SEA_PA));
    float t = time_to(&s, s.t_apogee, 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 60.0f, speed_at(&s, t));
    fsim_altitude(&s, t + 30.0f);
    TEST_ASSERT_EQUAL_INT(FSIM_LANDED, fsim_phase(&s));
}

/* [SIM-03] The channels are the bench's from the first start until reboot,
 * a flight stopped part-way included. */
void test_SIM_03_the_channels_stay_mocked_after_a_stop(void) {
    fsim_params_t p = high();
    TEST_ASSERT_FALSE(bench_flight_mocked());
    TEST_ASSERT_EQUAL_INT(BF_STARTED, bench_flight_start(&p, SEA_PA, true, true, 0));
    TEST_ASSERT_TRUE(bench_flight_mocked());
    TEST_ASSERT_FALSE(bench_flight_fired(1));
    TEST_ASSERT_FALSE(bench_flight_firing(1000u));
    bench_flight_fire(1, 1000u);
    TEST_ASSERT_TRUE_MESSAGE(bench_flight_firing(1000u), "a mocked fire the machine reads as refused");
    TEST_ASSERT_TRUE(bench_flight_firing(1000u + BENCH_PULSE_MS - 1u));
    TEST_ASSERT_FALSE_MESSAGE(bench_flight_firing(1000u + BENCH_PULSE_MS), "the pulse never ends");
    TEST_ASSERT_TRUE_MESSAGE(bench_flight_fired(1), "a fired channel reads open, as a lit charge does");
    TEST_ASSERT_FALSE(bench_flight_fired(2));
    bench_flight_stop();
    float pa;
    TEST_ASSERT_FALSE(bench_flight_pressure(1000u, false, &pa));
    TEST_ASSERT_TRUE_MESSAGE(bench_flight_mocked(), "a stop gave the channels back mid-flight");
    bench_flight_fire(2, 5000u);
    bench_flight_fire(3, 5000u);
    bench_flight_status_t st;
    bench_flight_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1, st.fires[0]);
    TEST_ASSERT_EQUAL_UINT32(1, st.fires[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SIM_02_isa_pressure_at_the_layer_bases);
    RUN_TEST(test_SIM_02_isa_pressure_inside_the_layers);
    RUN_TEST(test_SIM_02_isa_density_at_sea_level_and_30_km);
    RUN_TEST(test_SIM_02_isa_altitude_inverts_pressure);
    RUN_TEST(test_SIM_02_the_coast_peaks_at_the_apogee_asked_for);
    RUN_TEST(test_SIM_02_phases_run_in_order_and_it_lands);
    RUN_TEST(test_SIM_02_descent_times_at_constant_rates);
    RUN_TEST(test_SIM_02_thin_air_speeds_the_drogue);
    RUN_TEST(test_SIM_02_a_high_pad_adds_its_own_altitude);
    RUN_TEST(test_SIM_01_profiles_that_cannot_fly_are_refused);
    RUN_TEST(test_SIM_01_a_bench_flight_starts_only_from_the_pad_in_test_mode);
    RUN_TEST(test_SIM_02_the_bench_replaces_the_reading_while_it_flies);
    RUN_TEST(test_SIM_02_the_bench_ends_when_both_have_landed);
    RUN_TEST(test_SIM_04_the_descent_starts_from_rest);
    RUN_TEST(test_SIM_04_a_failed_drogue_falls_ballistic_until_the_main);
    RUN_TEST(test_SIM_04_a_failed_main_stays_at_the_drogues_rate);
    RUN_TEST(test_SIM_04_with_both_failed_it_falls_ballistic_to_the_ground);
    RUN_TEST(test_SIM_03_the_channels_stay_mocked_after_a_stop);
    return UNITY_END();
}
