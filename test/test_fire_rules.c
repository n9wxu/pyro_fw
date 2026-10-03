/*
 * The fire rules, flown [SYS-DEPLOY-05, PYR-FIRE-01, PYR-REFIRE-01,
 * FLT-EMRG-01..05, PYR-DEPLOY-02, PYR-HEALTH-01, PYR-BOARD-01..04, TST-04]:
 * charges that do not light, canopies that fail, and what the board does
 * about each.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board_harness.h"
#include "flight_run.h"
#include "../src/pad_check.h"

void setUp(void) {}
void tearDown(void) {}

/* A drogue at apogee that re-fires above 30 m/s, a main at 150 m that
 * re-fires above 12 m/s, and the emergency fire at 45 m/s. */
#define RULES                                                                                                          \
    "pyro1_mode=delay\npyro1_value=0\npyro2_mode=agl\npyro2_value=150\n"                                               \
    "pyro1_refire_speed=30\npyro2_refire_speed=12\nemergency_fire_speed=45\n"

static int pulses_on(const flown_t *r, uint8_t channel) {
    int n = 0;
    for (int i = 0; i < r->pulses; i++)
        n += r->pulse[i].channel == channel;
    return n;
}

static const flown_pulse_t *nth_pulse_on(const flown_t *r, uint8_t channel, int nth) {
    for (int i = 0; i < r->pulses; i++)
        if (r->pulse[i].channel == channel && --nth == 0)
            return &r->pulse[i];
    return NULL;
}

static const mp_rocket_t *rocket(void) {
    return &ROCKETS[SUBSONIC].r;
}

/* ── A clean flight is left alone ─────────────────────────────────── */

/* [SNS-EST-03] A drogue at 20 m/s and a main at 6 m/s, each well inside its
 * rule: every extra pulse comes in the second or two a canopy takes to slow
 * the rocket, and none after. */
void test_PYR_REFIRE_01_a_working_flight_gets_no_lasting_refire(void) {
    for (uint32_t seed = 1; seed <= 5; seed++) {
        flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .sensor_rms_pa = 5.0f};
        flown_t r = fly(rocket(), &ISA, &c, seed, 200.0f);
        char msg[96];
        snprintf(msg, sizeof(msg), "seed %u: %d pulses on pyro 1, %d on pyro 2", (unsigned)seed, pulses_on(&r, 1),
                 pulses_on(&r, 2));
        TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.main, msg);
        TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 1) == 1, msg);
        TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 2) <= 3, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, harness_log_events("EMERGENCY_FIRE"), msg);
        const flown_pulse_t *last = nth_pulse_on(&r, 2, pulses_on(&r, 2));
        TEST_ASSERT_TRUE_MESSAGE(last->t - r.main_t <= 3.0f, "a re-fire long after the main had opened");
    }
}

/* ── A charge that does not light [PYR-REFIRE-01] ─────────────────── */

void test_PYR_REFIRE_01_a_charge_that_lights_on_the_third_pulse(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .lights_on_pulse = {3, 0}};
    flown_t r = fly(rocket(), &ISA, &c, 2, 200.0f);
    const flown_pulse_t *second = nth_pulse_on(&r, 1, 2), *third = nth_pulse_on(&r, 1, 3);
    char msg[160];
    snprintf(msg, sizeof(msg), "%d pulses on pyro 1; fastest descent %.1f m/s", pulses_on(&r, 1),
             (double)r.fastest_descent_ms);
    TEST_ASSERT_NOT_NULL_MESSAGE(third, msg);
    /* The first re-fire waits for the fall to pass 30 m/s; then one a second. */
    TEST_ASSERT_TRUE_MESSAGE(-second->v >= 28.0f, "re-fired before the rocket was falling at the channel's speed");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 1.0f, third->t - second->t, msg);
    TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 1) <= 5, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.fastest_descent_ms < 45.0f, "the canopy opened before the emergency speed");
    TEST_ASSERT_EQUAL_INT(pulses_on(&r, 1) - 1, harness_log_events("PYRO1_REFIRE"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, harness_log_events("EMERGENCY_FIRE"), msg);
    TEST_ASSERT_TRUE_MESSAGE(r.main && fabsf(r.main_h - 150.0f) <= 8.0f, "the main still comes at its own height");
}

/* [SNS-EST-04] A drogue lost on the way down: the re-fire follows the true
 * speed past the channel's speed within two seconds. */
void test_SNS_EST_04_a_lost_canopy_is_seen_within_two_seconds(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .canopy_lost_below_m = 500.0f};
    flown_t r = fly(rocket(), &ISA, &c, 3, 200.0f);
    const flown_pulse_t *refire = nth_pulse_on(&r, 1, 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(refire, "the lost drogue was never re-fired");
    /* From 20 m/s, free fall passes 30 m/s about a second after the loss. */
    float lost_h = 500.0f, fell = lost_h - refire->h;
    char msg[96];
    snprintf(msg, sizeof(msg), "re-fired %.0f m below the loss, at %.1f m/s", (double)fell, (double)-refire->v);
    TEST_ASSERT_TRUE_MESSAGE(-refire->v >= 29.0f && -refire->v <= 30.0f + 2.0f * 9.81f, msg);
}

/* ── The emergency fire [FLT-EMRG-01, FLT-EMRG-04] ────────────────── */

void test_FLT_EMRG_01_no_canopy_fires_everything_and_keeps_firing(void) {
    flight_conditions_t c = {.config = RULES, .lights_on_pulse = {-1, -1}};
    flown_t r = fly(&ROCKETS[LOW_DRAG].r, &ISA, &c, 4, 400.0f);
    const flown_pulse_t *main_first = nth_pulse_on(&r, 2, 1);
    char msg[160];
    snprintf(msg, sizeof(msg), "pyro 1 %d pulses, pyro 2 %d pulses", pulses_on(&r, 1), pulses_on(&r, 2));
    TEST_ASSERT_NOT_NULL_MESSAGE(main_first, msg);
    TEST_ASSERT_TRUE_MESSAGE(main_first->h > 300.0f, "pyro 2 fired well above its own trigger");
    TEST_ASSERT_TRUE_MESSAGE(-main_first->v >= 44.0f, "and not before the emergency speed");
    TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 1) >= 4 && pulses_on(&r, 2) >= 4, msg);
    TEST_ASSERT_TRUE_MESSAGE(harness_log_events("EMERGENCY_FIRE") >= 1, "the log has no EMERGENCY_FIRE");
    TEST_ASSERT_EQUAL_INT_MESSAGE(pulses_on(&r, 1), ctx.fire.channel[0].pulses, "the count of pulses is reported");
    TEST_ASSERT_TRUE_MESSAGE(ctx.emergency_fire, "and the emergency, while it lasts");
    /* [PYR-DEPLOY-02] Never two channels within the gap of each other: a
     * 500 ms pulse, then 3 s of quiet. */
    for (int i = 1; i < r.pulses; i++) {
        if (r.pulse[i].channel != r.pulse[i - 1].channel)
            TEST_ASSERT_TRUE_MESSAGE(r.pulse[i].t - r.pulse[i - 1].t >= 3.45f, "two channels closer than the gap");
    }
}

/* A main that opens on the emergency fire ends it. */
void test_FLT_EMRG_01_a_canopy_that_opens_ends_it(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .lights_on_pulse = {-1, 1}};
    flown_t r = fly(rocket(), &ISA, &c, 5, 200.0f);
    const flown_pulse_t *last = &r.pulse[r.pulses - 1];
    char msg[96];
    snprintf(msg, sizeof(msg), "%d pulses, the last %.1f s after pyro 2's first", r.pulses,
             (double)(last->t - r.main_t));
    TEST_ASSERT_TRUE_MESSAGE(r.main, msg);
    TEST_ASSERT_TRUE_MESSAGE(last->t - r.main_t <= 8.0f, msg);
    TEST_ASSERT_FALSE_MESSAGE(ctx.emergency_fire, "the emergency ends when the speed drops");
}

/* [FLT-EMRG-01] Zero disables it: the same flight with no emergency speed
 * leaves pyro 2 for its own trigger. */
void test_FLT_EMRG_01_zero_leaves_each_channel_to_its_trigger(void) {
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro2_mode=agl\npyro2_value=150\n",
                             .lights_on_pulse = {-1, -1}};
    flown_t r = fly(rocket(), &ISA, &c, 6, 200.0f);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 1));
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 2));
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 150.0f, r.main_h);
}

/* ── Thin air [FLT-AIR-01] ────────────────────────────────────────── */

void test_FLT_AIR_01_a_good_drogue_at_20_km_is_not_a_failed_one(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .thin_air = true};
    flown_t r = fly(&ROCKETS[TO_20_KM].r, &ISA, &c, 7, 1500.0f);
    char msg[128];
    snprintf(msg, sizeof(msg), "apogee %.0f m; pyro 1 %d pulses; pyro 2 first at %.0f m", (double)r.apogee_h,
             pulses_on(&r, 1), (double)r.main_h);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_h > 18000.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 1) <= 2, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, harness_log_events("EMERGENCY_FIRE"), msg);
    TEST_ASSERT_TRUE_MESSAGE(r.main && fabsf(r.main_h - 150.0f) <= 15.0f, msg);
}

void test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .lights_on_pulse = {-1, 1}, .thin_air = true};
    flown_t r = fly(&ROCKETS[TO_20_KM].r, &ISA, &c, 8, 1500.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "pyro 2 first at %.0f m of %.0f m", (double)r.main_h, (double)r.apogee_h);
    TEST_ASSERT_TRUE_MESSAGE(r.main && r.main_h > 5000.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(harness_log_events("EMERGENCY_FIRE") >= 1, msg);
}

/* ── Nothing withholds a fire [SYS-DEPLOY-05, PYR-HEALTH-01, PYR-FIRE-01] ── */

/* Both channels read faulted, on the pad and all through the flight: the pad
 * says so, and each channel still fires at its trigger. */
void test_PYR_HEALTH_01_a_faulted_channel_still_fires(void) {
    flight_conditions_t c = {.config = RULES, .rate_ms = {20.0f, 6.0f}, .pyro_faulted = {true, true}};
    flown_t r = fly(rocket(), &ISA, &c, 9, 200.0f);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_PYRO_1) && (ctx.diag & DIAG_PYRO_2), "both faults are diagnosed");
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 1.5f,
                             "pyro 1 fires at apogee all the same");
    TEST_ASSERT_TRUE_MESSAGE(r.main && fabsf(r.main_h - 150.0f) <= 8.0f, "and pyro 2 at its height");
}

/* The board takes every command and energises nothing: every pulse is still
 * commanded and recorded, with the fault beside it, and the rules go on
 * asking. Nothing is refused. */
void test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused(void) {
    flight_conditions_t c = {.config = RULES, .lights_on_pulse = {-1, -1}, .energises_nothing = true};
    flown_t r = fly(rocket(), &ISA, &c, 10, 200.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "pyro 1 %d pulses, pyro 2 %d pulses", pulses_on(&r, 1), pulses_on(&r, 2));
    TEST_ASSERT_TRUE_MESSAGE(pulses_on(&r, 1) >= 2 && pulses_on(&r, 2) >= 1, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_log_events("PYRO1"), "the first pulse is in the log");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_log_events("PYRO1_FAULT"), "with what the board observed of it");
    TEST_ASSERT_TRUE(ctx.channel_fault[0] && ctx.channel_fault[1]);
    TEST_ASSERT_EQUAL_INT(0, harness_log_events("PYRO1_REFUSED"));
}

/* ── The board's limits [PYR-BOARD-01..04] ────────────────────────── */

/* With neither value configured the board's own defaults are in force. */
void test_PYR_BOARD_01_a_board_redefines_the_defaults(void) {
    static const pyro_limits_t slow_ptc = {2000, 2000, 10000, 6000, 6000, 10000};
    flight_conditions_t c = {.config = RULES, .lights_on_pulse = {-1, -1}, .limits = &slow_ptc};
    flown_t r = fly(&ROCKETS[LOW_DRAG].r, &ISA, &c, 11, 400.0f);
    TEST_ASSERT_EQUAL_UINT32(2000, ctx.plan.refire_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(6000, ctx.plan.fire_gap_ms);
    TEST_ASSERT_FALSE(ctx.refire_interval_limited || ctx.fire_gap_limited);
    for (int i = 1; i < r.pulses; i++) {
        float apart = r.pulse[i].t - r.pulse[i - 1].t;
        if (r.pulse[i].channel != r.pulse[i - 1].channel)
            TEST_ASSERT_TRUE_MESSAGE(apart >= 6.45f, "two channels closer than the board's gap");
        else
            TEST_ASSERT_TRUE_MESSAGE(apart >= 1.95f, "a re-fire sooner than the board's interval");
    }
    TEST_ASSERT_TRUE(r.pulses >= 4);
}

/* A value outside the board's range is brought to the nearest permitted one
 * and reported; the configuration is not rejected, and the flight still
 * fires [PYR-BOARD-04]. */
void test_PYR_BOARD_02_a_value_out_of_range_is_brought_in_and_reported(void) {
    flight_conditions_t c = {.config = RULES "fire_gap=200\nrefire_interval=60000\n", .lights_on_pulse = {-1, -1}};
    flown_t r = fly(&ROCKETS[LOW_DRAG].r, &ISA, &c, 12, 400.0f);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000, ctx.plan.fire_gap_ms, "raised to the least the board permits");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(10000, ctx.plan.refire_interval_ms, "lowered to the most it permits");
    TEST_ASSERT_TRUE(ctx.fire_gap_limited && ctx.refire_interval_limited);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.main, "and both channels still fire");
    for (int i = 1; i < r.pulses; i++) {
        if (r.pulse[i].channel != r.pulse[i - 1].channel)
            TEST_ASSERT_TRUE_MESSAGE(r.pulse[i].t - r.pulse[i - 1].t >= 1.45f, "two channels closer than 1 s of quiet");
    }
}

/* A value inside the range is the operator's. */
void test_PYR_BOARD_01_a_configured_value_in_range_is_used(void) {
    flight_conditions_t c = {.config = RULES "fire_gap=1500\nrefire_interval=2500\n", .lights_on_pulse = {-1, -1}};
    (void)fly(rocket(), &ISA, &c, 13, 60.0f);
    TEST_ASSERT_EQUAL_UINT32(1500, ctx.plan.fire_gap_ms);
    TEST_ASSERT_EQUAL_UINT32(2500, ctx.plan.refire_interval_ms);
    TEST_ASSERT_FALSE(ctx.fire_gap_limited || ctx.refire_interval_limited);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PYR_REFIRE_01_a_working_flight_gets_no_lasting_refire);
    RUN_TEST(test_PYR_REFIRE_01_a_charge_that_lights_on_the_third_pulse);
    RUN_TEST(test_SNS_EST_04_a_lost_canopy_is_seen_within_two_seconds);
    RUN_TEST(test_FLT_EMRG_01_no_canopy_fires_everything_and_keeps_firing);
    RUN_TEST(test_FLT_EMRG_01_a_canopy_that_opens_ends_it);
    RUN_TEST(test_FLT_EMRG_01_zero_leaves_each_channel_to_its_trigger);
    RUN_TEST(test_FLT_AIR_01_a_good_drogue_at_20_km_is_not_a_failed_one);
    RUN_TEST(test_FLT_AIR_01_a_failed_drogue_at_20_km_is_still_seen);
    RUN_TEST(test_PYR_HEALTH_01_a_faulted_channel_still_fires);
    RUN_TEST(test_PYR_FIRE_01_a_pulse_that_energises_nothing_is_recorded_not_refused);
    RUN_TEST(test_PYR_BOARD_01_a_board_redefines_the_defaults);
    RUN_TEST(test_PYR_BOARD_02_a_value_out_of_range_is_brought_in_and_reported);
    RUN_TEST(test_PYR_BOARD_01_a_configured_value_in_range_is_used);
    return UNITY_END();
}
