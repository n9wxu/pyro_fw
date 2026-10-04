/*
 * The fire rules as a pure decision, through fire_control.h: which channel
 * to pulse, and when [PYR-MODE-01..06, PYR-DEPLOY-02, PYR-REFIRE-01,
 * FLT-EMRG-01, PYR-SAFE-03, PYR-SAFE-04].
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-EMRG-05, FLT-ASC-02].
 */
#include "unity.h"
#include "../src/atmosphere.h"
#include "../src/config.h"
#include "../src/fire_control.h"
#include <math.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f
#define PULSE_MS 500u
#define STEP_MS 20u

/* A descent at a steady speed from a peak, with the board's pulse timing:
 * the decision is stepped every sample and each command is delivered. */
typedef struct {
    fire_state_t state;
    fire_plan_t plan;
    uint32_t now_ms;
    float height_m, speed_ms; /* above the pad; descending is positive */
    float peak_m;
    bool apogee;
    uint32_t apogee_ms;
    bool known;
    uint32_t pulse_until_ms;
    bool pulsing;
    struct {
        uint32_t ms;
        uint8_t channel;
        fire_kind_t kind;
    } log[64];
    int pulses;
} rig_t;

static void rig_start(rig_t *r) {
    memset(r, 0, sizeof(*r));
    fire_control_reset(&r->state);
    r->plan.refire_interval_ms = 1000;
    r->plan.fire_gap_ms = 3000;
    r->height_m = 1000.0f;
    r->peak_m = 1000.0f;
    r->known = true;
    r->now_ms = 100000;
}

static fire_inputs_t rig_inputs(const rig_t *r) {
    float pa = atmos_pressure_above_pa(PAD_PA, r->height_m);
    fire_inputs_t in = {
        .now_ms = r->now_ms,
        .apogee_declared = r->apogee,
        .apogee_ms = r->apogee_ms,
        .state_known = r->known,
        .pressure_pa = pa,
        .rate = r->speed_ms / atmos_scale_height_m(pa),
        .pad_pa = PAD_PA,
        .peak_pa = atmos_pressure_above_pa(PAD_PA, r->peak_m),
        .pulse_active = r->pulsing,
    };
    return in;
}

static void rig_step(rig_t *r) {
    r->now_ms += STEP_MS;
    r->height_m -= r->speed_ms * (float)STEP_MS / 1000.0f;
    if (r->pulsing && r->now_ms >= r->pulse_until_ms)
        r->pulsing = false;
    fire_inputs_t in = rig_inputs(r);
    fire_command_t c = fire_control_step(&r->state, &r->plan, &in);
    if (c.kind == FIRE_NONE)
        return;
    fire_control_pulse_started(&r->state, c.channel, &in);
    r->pulsing = true;
    r->pulse_until_ms = r->now_ms + PULSE_MS;
    if (r->pulses < 64) {
        r->log[r->pulses].ms = r->now_ms;
        r->log[r->pulses].channel = c.channel;
        r->log[r->pulses].kind = c.kind;
        r->pulses++;
    }
}

static void rig_run(rig_t *r, uint32_t ms) {
    for (uint32_t end = r->now_ms + ms; r->now_ms < end;)
        rig_step(r);
}

static void rig_apogee(rig_t *r) {
    r->apogee = true;
    r->apogee_ms = r->now_ms;
}

static int pulses_on(const rig_t *r, uint8_t channel) {
    int n = 0;
    for (int i = 0; i < r->pulses; i++)
        n += r->log[i].channel == channel;
    return n;
}

static void channel(rig_t *r, int i, uint8_t mode, float metres_or_ms, uint32_t delay_ms) {
    r->plan.enabled[i] = true;
    r->plan.mode[i] = mode;
    r->plan.trigger_m[i] = metres_or_ms;
    r->plan.trigger_ms[i] = metres_or_ms;
    r->plan.trigger_delay_ms[i] = delay_ms;
}

/* ── Before apogee nothing fires ──────────────────────────────────── */

void test_PYR_SAFE_04_nothing_fires_before_apogee(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_AGL, 5000.0f, 0);
    r.plan.emergency_ms = 10.0f;
    r.speed_ms = 80.0f; /* every trigger and the emergency speed are met */
    rig_run(&r, 5000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.pulses, "a trigger met before apogee fires nothing");
}

/* ── Each mode's trigger ──────────────────────────────────────────── */

void test_PYR_MODE_01_delay_counts_from_apogee(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 3000);
    r.speed_ms = 5.0f;
    rig_apogee(&r);
    uint32_t apogee = r.now_ms;
    rig_run(&r, 6000);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
    TEST_ASSERT_UINT32_WITHIN(STEP_MS, apogee + 3000, r.log[0].ms);
    TEST_ASSERT_EQUAL(FIRE_FIRST, r.log[0].kind);
}

void test_PYR_MODE_02_agl_fires_on_descending_through_the_height(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 1, PYRO_MODE_AGL, 300.0f, 0);
    r.speed_ms = 20.0f;
    rig_apogee(&r);
    rig_run(&r, 60000);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
    float fired_at_m = 1000.0f - 20.0f * (float)(r.log[0].ms - 100000u) / 1000.0f;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 300.0f, fired_at_m);
}

void test_PYR_MODE_03_fallen_measures_from_the_peak(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_FALLEN, 150.0f, 0);
    r.speed_ms = 15.0f;
    rig_apogee(&r);
    rig_run(&r, 30000);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
    float fired_at_m = 1000.0f - 15.0f * (float)(r.log[0].ms - 100000u) / 1000.0f;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 850.0f, fired_at_m);
}

void test_PYR_MODE_04_speed_fires_at_the_descent_speed(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_SPEED, 25.0f, 0);
    rig_apogee(&r);
    r.speed_ms = 24.0f;
    rig_run(&r, 2000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.pulses, "a metre a second short of the set speed");
    r.speed_ms = 26.0f;
    rig_run(&r, 200);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
}

/* [SNS-PRES-10, SNS-PRES-11] With no data a pressure trigger waits; a DELAY,
 * whose apogee is known, does not [SYS-DEPLOY-04]. */
void test_SYS_DEPLOY_04_without_data_only_a_delay_fires(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 1000);
    channel(&r, 1, PYRO_MODE_AGL, 5000.0f, 0);
    r.plan.emergency_ms = 10.0f;
    r.speed_ms = 60.0f;
    r.known = false;
    rig_apogee(&r);
    rig_run(&r, 5000);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pulses_on(&r, 2), "an AGL trigger and the emergency rule wait for data");
}

/* ── One fire per channel unless a rule asks again ────────────────── */

void test_PYR_SAFE_03_a_channel_fires_once_when_no_rule_asks_again(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    r.speed_ms = 40.0f; /* fast, but both speed rules are off */
    rig_apogee(&r);
    rig_run(&r, 10000);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
}

/* ── Re-fire [PYR-REFIRE-01] ──────────────────────────────────────── */

void test_PYR_REFIRE_01_refires_every_interval_while_too_fast(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 30.0f;
    r.speed_ms = 40.0f;
    rig_apogee(&r);
    rig_run(&r, 5100);
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, r.pulses, "the first fire, then one each second for five seconds");
    for (int i = 1; i < r.pulses; i++) {
        TEST_ASSERT_EQUAL(FIRE_REFIRE, r.log[i].kind);
        TEST_ASSERT_UINT32_WITHIN(STEP_MS, 1000u, r.log[i].ms - r.log[i - 1].ms);
    }
}

void test_PYR_REFIRE_01_stops_when_the_descent_slows(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 30.0f;
    r.speed_ms = 40.0f;
    rig_apogee(&r);
    rig_run(&r, 2500);
    int while_fast = r.pulses;
    r.speed_ms = 20.0f;
    rig_run(&r, 5000);
    TEST_ASSERT_EQUAL_INT(3, while_fast);
    TEST_ASSERT_EQUAL_INT_MESSAGE(while_fast, r.pulses, "no re-fire below the channel's speed");
    r.speed_ms = 35.0f; /* the canopy is lost again */
    rig_run(&r, 1100);
    TEST_ASSERT_TRUE_MESSAGE(r.pulses > while_fast, "and it starts again if the speed returns");
}

void test_PYR_REFIRE_01_zero_disables_it(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 0.0f;
    r.speed_ms = 90.0f;
    rig_apogee(&r);
    rig_run(&r, 8000);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
}

void test_PYR_REFIRE_01_each_channel_has_its_own_speed(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 30.0f;
    r.plan.refire_ms[1] = 8.0f;
    r.speed_ms = 15.0f; /* slow enough for pyro 1, too fast for pyro 2 */
    rig_apogee(&r);
    rig_run(&r, 12000);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 1));
    TEST_ASSERT_TRUE(pulses_on(&r, 2) >= 4);
}

/* [FLT-AIR-01] The rules read the descent as the pad's air would give it: a
 * canopy good for 25 m/s at the pad falls at about 115 m/s at 30 km. */
void test_FLT_AIR_01_a_good_canopy_in_thin_air_is_not_refired(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 30.0f;
    r.plan.emergency_ms = 50.0f;
    r.height_m = 30000.0f;
    r.peak_m = 30000.0f;
    float thin = atmos_pad_air_ratio(atmos_pressure_above_pa(PAD_PA, 30000.0f), PAD_PA);
    r.speed_ms = 25.0f / thin;
    TEST_ASSERT_TRUE_MESSAGE(r.speed_ms > 100.0f, "the true speed is far above both rules' speeds");
    rig_apogee(&r);
    rig_run(&r, 6000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.pulses, "only the trigger's own fire");
    r.speed_ms = 60.0f / thin; /* and a failed canopy up there is still seen */
    rig_run(&r, 1200);
    TEST_ASSERT_TRUE(r.pulses > 1);
}

/* ── Emergency all-fire [FLT-EMRG-01] ─────────────────────────────── */

void test_FLT_EMRG_01_every_enabled_channel_fires_whatever_its_trigger(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 600000);
    channel(&r, 1, PYRO_MODE_AGL, 100.0f, 0);
    r.plan.emergency_ms = 50.0f;
    r.speed_ms = 30.0f;
    rig_apogee(&r);
    rig_run(&r, 3000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.pulses, "below the emergency speed neither trigger is met");
    r.speed_ms = 55.0f;
    rig_run(&r, 4500);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 1));
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 2));
    TEST_ASSERT_EQUAL(FIRE_EMERGENCY, r.log[0].kind);
    TEST_ASSERT_EQUAL(FIRE_EMERGENCY, r.log[1].kind);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.log[0].channel, "pyro 1 first on a tie");
}

void test_FLT_EMRG_01_fires_again_until_the_speed_drops(void) {
    rig_t r;
    rig_start(&r);
    r.height_m = 3000.0f;
    r.peak_m = 3000.0f;
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_AGL, 100.0f, 0);
    r.plan.emergency_ms = 50.0f;
    r.speed_ms = 60.0f;
    rig_apogee(&r);
    rig_run(&r, 20000);
    int p1 = pulses_on(&r, 1), p2 = pulses_on(&r, 2);
    TEST_ASSERT_TRUE_MESSAGE(p1 >= 3 && p2 >= 2, "both channels keep firing");
    r.speed_ms = 20.0f;
    rig_run(&r, 8000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(p1, pulses_on(&r, 1), "and stop below the emergency speed");
    TEST_ASSERT_EQUAL_INT(p2, pulses_on(&r, 2));
}

void test_FLT_EMRG_01_zero_disables_it(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_AGL, 100.0f, 0);
    r.plan.emergency_ms = 0.0f;
    r.speed_ms = 200.0f;
    r.height_m = 20000.0f;
    r.peak_m = 20000.0f;
    rig_apogee(&r);
    rig_run(&r, 8000);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 1));
    TEST_ASSERT_EQUAL_INT(0, pulses_on(&r, 2));
}

void test_PYR_HEALTH_02_a_channel_that_is_not_enabled_never_fires(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.enabled[1] = false;
    r.plan.emergency_ms = 30.0f;
    r.plan.refire_ms[1] = 10.0f;
    r.speed_ms = 60.0f;
    rig_apogee(&r);
    rig_run(&r, 6000);
    TEST_ASSERT_TRUE(pulses_on(&r, 1) >= 1);
    TEST_ASSERT_EQUAL_INT(0, pulses_on(&r, 2));
}

/* ── The gap and the order [PYR-DEPLOY-02] ────────────────────────── */

void test_PYR_DEPLOY_02_never_together_and_the_gap_is_quiet_time(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_DELAY, 0.0f, 0); /* both on one event [PYR-DEPLOY-01] */
    r.speed_ms = 5.0f;
    rig_apogee(&r);
    rig_run(&r, 8000);
    TEST_ASSERT_EQUAL_INT(2, r.pulses);
    TEST_ASSERT_EQUAL_UINT8(1, r.log[0].channel);
    TEST_ASSERT_EQUAL_UINT8(2, r.log[1].channel);
    /* From the end of the first pulse to the start of the second. */
    TEST_ASSERT_UINT32_WITHIN(STEP_MS, PULSE_MS + 3000u, r.log[1].ms - r.log[0].ms);
}

void test_PYR_DEPLOY_02_a_first_fire_goes_before_a_refire(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_AGL, 900.0f, 0);
    r.plan.refire_ms[0] = 20.0f;
    r.speed_ms = 25.0f; /* pyro 1 re-fires every second; pyro 2's height comes 4 s in */
    rig_apogee(&r);
    rig_run(&r, 12000);
    int first2 = -1;
    for (int i = 0; i < r.pulses && first2 < 0; i++)
        if (r.log[i].channel == 2)
            first2 = i;
    TEST_ASSERT_TRUE_MESSAGE(first2 > 0, "pyro 2 fired after pyro 1 had begun re-firing");
    float trigger_s = (1000.0f - 900.0f) / 25.0f;
    float fired_s = (float)(r.log[first2].ms - 100000u) / 1000.0f;
    /* It waits only for the pulse in progress and the gap after it: pyro 1's
     * re-fires stand off rather than keep the path busy. */
    TEST_ASSERT_TRUE_MESSAGE(fired_s - trigger_s <= (float)(PULSE_MS + 3000u) / 1000.0f + 0.1f,
                             "the first fire waited longer than a pulse and a gap");
    TEST_ASSERT_UINT32_WITHIN(STEP_MS, PULSE_MS + 3000u, r.log[first2].ms - r.log[first2 - 1].ms);
}

void test_PYR_DEPLOY_02_two_refiring_channels_take_turns(void) {
    rig_t r;
    rig_start(&r);
    r.height_m = 5000.0f;
    r.peak_m = 5000.0f;
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_DELAY, 0.0f, 0);
    r.plan.refire_ms[0] = 20.0f;
    r.plan.refire_ms[1] = 20.0f;
    r.speed_ms = 40.0f;
    rig_apogee(&r);
    rig_run(&r, 30000);
    TEST_ASSERT_TRUE(r.pulses >= 8);
    for (int i = 1; i < r.pulses; i++) {
        TEST_ASSERT_TRUE_MESSAGE(r.log[i].channel != r.log[i - 1].channel, "neither channel starves the other");
        TEST_ASSERT_TRUE_MESSAGE(r.log[i].ms - r.log[i - 1].ms >= PULSE_MS + 3000u - STEP_MS,
                                 "and the gap separates every pair");
    }
}

/* ── A charge in the bay [PYR-MODE-06] ────────────────────────────── */

/* The first charge pressurises the bay: for a moment the pressure reads the
 * rocket hundreds of metres lower and falling at hundreds of metres a
 * second. Neither a trigger nor a speed rule may act on it. */
void test_PYR_MODE_06_a_charge_does_not_bring_a_trigger_forward(void) {
    rig_t r;
    rig_start(&r);
    channel(&r, 0, PYRO_MODE_DELAY, 0.0f, 0);
    channel(&r, 1, PYRO_MODE_AGL, 700.0f, 0);
    r.plan.emergency_ms = 50.0f;
    r.plan.refire_ms[0] = 30.0f;
    r.speed_ms = 2.0f;
    rig_apogee(&r);
    rig_run(&r, 100);
    TEST_ASSERT_EQUAL_INT(1, r.pulses);
    /* The bay, 400 m "lower" and "falling" at 300 m/s, for half a second. */
    float true_height = r.height_m;
    for (int i = 0; i < 25; i++) {
        r.height_m = true_height - 400.0f;
        r.speed_ms = 300.0f;
        r.now_ms += STEP_MS;
        if (r.pulsing && r.now_ms >= r.pulse_until_ms)
            r.pulsing = false;
        fire_inputs_t in = rig_inputs(&r);
        fire_command_t c = fire_control_step(&r.state, &r.plan, &in);
        TEST_ASSERT_EQUAL_MESSAGE(FIRE_NONE, c.kind, "nothing fires on the charge's pressure");
    }
    r.height_m = true_height;
    r.speed_ms = 20.0f;
    rig_run(&r, 30000);
    TEST_ASSERT_EQUAL_INT(1, pulses_on(&r, 2));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PYR_SAFE_04_nothing_fires_before_apogee);
    RUN_TEST(test_PYR_MODE_01_delay_counts_from_apogee);
    RUN_TEST(test_PYR_MODE_02_agl_fires_on_descending_through_the_height);
    RUN_TEST(test_PYR_MODE_03_fallen_measures_from_the_peak);
    RUN_TEST(test_PYR_MODE_04_speed_fires_at_the_descent_speed);
    RUN_TEST(test_SYS_DEPLOY_04_without_data_only_a_delay_fires);
    RUN_TEST(test_PYR_SAFE_03_a_channel_fires_once_when_no_rule_asks_again);
    RUN_TEST(test_PYR_REFIRE_01_refires_every_interval_while_too_fast);
    RUN_TEST(test_PYR_REFIRE_01_stops_when_the_descent_slows);
    RUN_TEST(test_PYR_REFIRE_01_zero_disables_it);
    RUN_TEST(test_PYR_REFIRE_01_each_channel_has_its_own_speed);
    RUN_TEST(test_FLT_AIR_01_a_good_canopy_in_thin_air_is_not_refired);
    RUN_TEST(test_FLT_EMRG_01_every_enabled_channel_fires_whatever_its_trigger);
    RUN_TEST(test_FLT_EMRG_01_fires_again_until_the_speed_drops);
    RUN_TEST(test_FLT_EMRG_01_zero_disables_it);
    RUN_TEST(test_PYR_HEALTH_02_a_channel_that_is_not_enabled_never_fires);
    RUN_TEST(test_PYR_DEPLOY_02_never_together_and_the_gap_is_quiet_time);
    RUN_TEST(test_PYR_DEPLOY_02_a_first_fire_goes_before_a_refire);
    RUN_TEST(test_PYR_DEPLOY_02_two_refiring_channels_take_turns);
    RUN_TEST(test_PYR_MODE_06_a_charge_does_not_bring_a_trigger_forward);
    return UNITY_END();
}
