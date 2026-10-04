/*
 * A restart in flight: whether the flight is resumed, and what a resumed
 * flight then does [FLT-BROWN-02..07, DD-086].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <stdio.h>
#include <string.h>
#include "board_harness.h"
#include "flight_run.h"
#include "../src/atmosphere.h"
#include "../src/flight_resume.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325

/* ── The decision [FLT-BROWN-02, FLT-BROWN-03] ────────────────────── */

void test_FLT_BROWN_02_above_the_ground_and_moving_is_a_flight(void) {
    TEST_ASSERT_EQUAL(RESUME_ASCENT, resume_assess(true, 20000, 6000));
    TEST_ASSERT_EQUAL(RESUME_DESCENT, resume_assess(true, 20000, -700));
    TEST_ASSERT_EQUAL(RESUME_DESCENT, resume_assess(true, 3100, -520));
    TEST_ASSERT_EQUAL_MESSAGE(RESUME_NOT_FLYING, resume_assess(false, 20000, 6000), "never without the record");
    TEST_ASSERT_EQUAL_MESSAGE(RESUME_NOT_FLYING, resume_assess(true, 500, 6000), "never at the recorded ground");
    TEST_ASSERT_EQUAL_MESSAGE(RESUME_NOT_FLYING, resume_assess(true, -4000, -900), "nor below it");
}

void test_FLT_BROWN_03_a_still_board_is_not_airborne(void) {
    TEST_ASSERT_EQUAL(RESUME_STILL, resume_assess(true, 20000, 0));
    TEST_ASSERT_EQUAL(RESUME_STILL, resume_assess(true, 3000000, 300));
    TEST_ASSERT_EQUAL(RESUME_STILL, resume_assess(true, 150000, -300));
}

void test_FLT_BROWN_01_a_damaged_record_is_no_record(void) {
    pad_record_t r;
    pad_record_fill(&r, PAD_PA, 1200);
    TEST_ASSERT_TRUE(pad_record_valid(&r));
    pad_record_t torn = r;
    torn.ground_pressure_pa ^= 0x100;
    TEST_ASSERT_FALSE(pad_record_valid(&torn));
    memset(&torn, 0xFF, sizeof(torn)); /* an erased sector */
    TEST_ASSERT_FALSE(pad_record_valid(&torn));
    memset(&torn, 0, sizeof(torn));
    TEST_ASSERT_FALSE(pad_record_valid(&torn));
}

/* ── A start that does not resume [FLT-BROWN-02, -03, -05] ────────── */

static uint32_t now;

static void wait_ms(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now++)
        tick(now);
}

static void start(uint32_t seed, bool record, float pressure_pa) {
    boot_like_hardware(seed);
    now = 0;
    if (record)
        write_pad_record(PAD_PA);
    mock_pressure.pressure_pa = pressure_pa;
}

void test_FLT_BROWN_05_no_record_is_reported(void) {
    start(1, false, 70000.0f);
    run_to_pad(&now);
    TEST_ASSERT_EQUAL_STRING("not resumed: no record", flight_resume_text(&ctx));
}

void test_FLT_BROWN_05_at_ground_level_is_reported(void) {
    start(2, true, (float)PAD_PA - 60.0f);
    run_to_pad(&now);
    TEST_ASSERT_EQUAL_STRING("not resumed: at ground level", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT(0, harness_log_events("RESUMED"));
}

void test_FLT_BROWN_05_on_usb_is_reported(void) {
    start(3, true, 70000.0f);
    harness_usb(true);
    for (; now < 20000u; now++) {
        mock_pressure.pressure_pa = 70000.0f - 4.0f * (float)now / 1000.0f * 70.0f; /* climbing hard */
        tick(now);
        TEST_ASSERT_TRUE_MESSAGE(ctx.current_state < ASCENT || ctx.current_state > LANDED, "no flight on USB [USB-01]");
    }
    TEST_ASSERT_EQUAL_STRING("not resumed: on USB", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
}

/* The sensor answers and then delivers nothing the board can use. */
void test_FLT_BROWN_05_no_sample_in_time_is_reported(void) {
    start(4, true, -50.0f);
    wait_ms(8000);
    TEST_ASSERT_EQUAL_STRING("not resumed: no sample in time", flight_resume_text(&ctx));
    TEST_ASSERT_TRUE_MESSAGE(ctx.current_state != BOOT_SENSOR, "the start does not hang on the decision");
}

/* [FLT-BROWN-03] High above the recorded ground and still: carried up a
 * mountain since the record was made. It goes to the pad and fires nothing. */
void test_FLT_BROWN_03_a_still_board_at_altitude_goes_to_the_pad(void) {
    start(5, true, 80000.0f);
    run_to_pad(&now);
    wait_ms(15000);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_EQUAL_STRING("not resumed: altitude unexplained", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_INT32_WITHIN(4, 80000, ctx.ground_pressure);
}

/* [FLT-BROWN-02] Two readings far above the ground, at the worst moment,
 * do not make a flight of a board on its pad. */
void test_FLT_BROWN_02_two_bad_readings_cannot_decide_it(void) {
    const int32_t off[] = {-20000, -60000, -3000};
    for (unsigned k = 0; k < 3; k++) {
        for (uint32_t at = 2500; at <= 3100; at += 100) {
            start(6 + k, true, (float)PAD_PA);
            for (; now < 20000u && ctx.current_state != PAD_IDLE; now++) {
                if (now == at) {
                    mock_glitch_pa = off[k];
                    mock_glitch_samples = 2;
                }
                tick(now);
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "%ld Pa at %lu ms", (long)off[k], (unsigned long)at);
            TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("not resumed: at ground level", flight_resume_text(&ctx), msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_pulse_count, msg);
        }
    }
}

/* ── A resumed flight [FLT-BROWN-02, -06, -07] ────────────────────── */

#define MAIN_M 150
/* A restarted board settles and then needs 0.6 s of readings. */
#define RESTART_S 4.0f
static const char RESUMED_CONFIG[] = "pyro2_mode=agl\npyro2_value=150\n";

static const flown_pulse_t *first_pulse_after(const flown_t *f, uint8_t channel, float t) {
    for (int i = 0; i < f->pulses; i++)
        if (f->pulse[i].channel == channel && f->pulse[i].t >= t)
            return &f->pulse[i];
    return NULL;
}

void test_FLT_BROWN_02_every_cause_of_restart_resumes_the_flight(void) {
    const reset_cause_t causes[] = {RESET_POWER_EVENT, RESET_RUN_PIN, RESET_SOFTWARE, RESET_DEBUG};
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    float coast = 0.5f * (r->burn_s + plant_apogee_s(r, &COLD));
    for (unsigned k = 0; k < 4; k++) {
        flight_conditions_t c = {.config = RESUMED_CONFIG,
                                 .rate_ms = {20.0f, 6.0f},
                                 .pad_s = 12.0f,
                                 .restart_at_s = coast,
                                 .restart_cause = (int)causes[k]};
        flown_t f = fly(r, &COLD, &c, 20 + k, 200.0f);
        char msg[48];
        snprintf(msg, sizeof(msg), "cause %d", (int)causes[k]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("resumed in ascent", flight_resume_text(&ctx), msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_log_events("RESUMED"), "recorded in the log [FLT-BROWN-07]");
        TEST_ASSERT_INT32_WITHIN_MESSAGE(4, (int32_t)mp_pad_pa(&COLD), ctx.ground_pressure, "the recorded ground");
        TEST_ASSERT_TRUE_MESSAGE(f.apogee_declared, msg);
        TEST_ASSERT_TRUE_MESSAGE(f.declared_t >= f.apogee_t, "apogee is never declared early");
        TEST_ASSERT_TRUE_MESSAGE(f.drogue && f.drogue_t >= f.apogee_t && f.drogue_t < f.apogee_t + 2.5f, msg);
        TEST_ASSERT_TRUE_MESSAGE(f.main, msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.15f * MAIN_M, (float)MAIN_M, f.main_h, msg);
    }
}

/* [FLT-BROWN-06] Restarted under the drogue, after pyro 1 fired: the channels
 * are taken as unfired, so pyro 1 fires again at once and pyro 2 at its height. */
void test_FLT_BROWN_06_a_descent_resumed_fires_every_channel_afresh(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    float at = plant_apogee_s(r, &COLD) + 6.0f;
    flight_conditions_t c = {
        .config = RESUMED_CONFIG, .rate_ms = {20.0f, 6.0f}, .pad_s = 12.0f, .restart_at_s = at};
    flown_t f = fly(r, &COLD, &c, 30, 200.0f);
    TEST_ASSERT_EQUAL_STRING("resumed in descent", flight_resume_text(&ctx));
    TEST_ASSERT_TRUE_MESSAGE(f.drogue_t < at, "pyro 1 had fired before the restart");
    const flown_pulse_t *again = first_pulse_after(&f, 1, at);
    TEST_ASSERT_NOT_NULL_MESSAGE(again, "pyro 1 fires again");
    TEST_ASSERT_TRUE_MESSAGE(again->t < at + RESTART_S, "as soon as fresh data meets its trigger");
    const flown_pulse_t *main = first_pulse_after(&f, 2, at);
    TEST_ASSERT_NOT_NULL(main);
    TEST_ASSERT_FLOAT_WITHIN(0.15f * MAIN_M, (float)MAIN_M, main->h);
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("RESUMED"));
}

/* [FLT-BROWN-06] Restarted below the main's height: both channels at once,
 * pyro 1 first and the gap kept [PYR-DEPLOY-02]. */
void test_FLT_BROWN_06_a_resume_below_the_main_height_fires_both(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    flight_conditions_t probe = {.config = RESUMED_CONFIG, .rate_ms = {20.0f, 6.0f}, .pad_s = 12.0f};
    flown_t clean = fly(r, &COLD, &probe, 31, 200.0f);
    float at = clean.main_t + 4.0f;
    flight_conditions_t c = probe;
    c.restart_at_s = at;
    flown_t f = fly(r, &COLD, &c, 31, 200.0f);
    const flown_pulse_t *p1 = first_pulse_after(&f, 1, at), *p2 = first_pulse_after(&f, 2, at);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_TRUE(p1->t < at + RESTART_S);
    TEST_ASSERT_TRUE_MESSAGE(p2->t > p1->t, "pyro 1 first");
    TEST_ASSERT_TRUE_MESSAGE(p2->t - p1->t >= 0.5f + 3.0f - 0.05f, "the gap, from the end of the first pulse");
    TEST_ASSERT_TRUE(p2->t - p1->t < 0.5f + 3.0f + 0.5f);
}

/* [FLT-BROWN-06] The time of apogee is lost with the restart, so a DELAY
 * counts its whole value from the resume. */
void test_FLT_BROWN_06_a_delay_counts_in_full_from_the_resume(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    float at = plant_apogee_s(r, &COLD) + 1.5f;
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro1_value=4\npyro2_mode=none\n",
                             .lights_on_pulse = {2, 0},
                             .pad_s = 12.0f,
                             .restart_at_s = at};
    flown_t f = fly(r, &COLD, &c, 32, 200.0f);
    TEST_ASSERT_EQUAL_STRING("resumed in descent", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.pulses, "none before the restart, one after");
    TEST_ASSERT_TRUE_MESSAGE(f.pulse[0].t >= at + 4.0f, "not before the full delay");
    TEST_ASSERT_TRUE(f.pulse[0].t < at + 4.0f + RESTART_S);
}

/* [FLT-BROWN-06] A flight resumed climbing waits for its own apogee: a DELAY
 * then counts from that. */
void test_FLT_BROWN_06_a_climb_resumed_never_fires_before_apogee(void) {
    const int rockets[] = {SUBSONIC, MID_MACH, LOW_DRAG};
    for (unsigned k = 0; k < 3; k++) {
        const mp_rocket_t *r = &ROCKETS[rockets[k]].r;
        float apogee = plant_apogee_s(r, &COLD);
        const float fractions[] = {0.3f, 0.6f, 0.9f};
        for (unsigned j = 0; j < 3; j++) {
            float at = r->burn_s + fractions[j] * (apogee - r->burn_s);
            flight_conditions_t c = {.pad_s = 12.0f, .restart_at_s = at, .stop_after_apogee = false};
            flown_t f = fly(r, &COLD, &c, 40 + 3 * k + j, apogee + 12.0f);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s restarted at %.1f s", ROCKETS[rockets[k]].name, (double)at);
            TEST_ASSERT_TRUE_MESSAGE(f.pulses >= 1, msg);
            TEST_ASSERT_TRUE_MESSAGE(f.pulse[0].t >= f.apogee_t, msg);
            TEST_ASSERT_TRUE_MESSAGE(f.pulse[0].t < f.apogee_t + 4.0f, msg);
        }
    }
}

/* [FLT-BROWN-06] The emergency fire applies from the resume: restarted in a
 * fall with nothing out, everything fires. */
void test_FLT_BROWN_06_the_emergency_fire_applies_from_the_resume(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    float at = plant_apogee_s(r, &COLD) + 5.0f;
    flight_conditions_t c = {.config = "pyro1_mode=delay\npyro1_value=60\npyro2_mode=agl\npyro2_value=20\n"
                                       "emergency_fire_speed=35\n",
                             .lights_on_pulse = {-1, -1},
                             .pad_s = 12.0f,
                             .restart_at_s = at};
    flown_t f = fly(r, &COLD, &c, 50, 200.0f);
    const flown_pulse_t *p1 = first_pulse_after(&f, 1, at), *p2 = first_pulse_after(&f, 2, at);
    TEST_ASSERT_NOT_NULL_MESSAGE(p1, "pyro 1, long before its delay");
    TEST_ASSERT_NOT_NULL_MESSAGE(p2, "pyro 2, far above its height");
    TEST_ASSERT_TRUE(p1->t < at + 60.0f && p2->h > 20.0f * 1.5f);
    TEST_ASSERT_TRUE(harness_log_events("EMERGENCY_FIRE") >= 1);
}

/* ── Afterwards [FLT-BROWN-04] ────────────────────────────────────── */

void test_FLT_BROWN_04_a_landing_clears_the_resume_state(void) {
    flight_conditions_t c = {.pad_s = 12.0f, .to_landed = true};
    flown_t f = fly(&ROCKETS[HOP].r, &COLD, &c, 60, 200.0f);
    TEST_ASSERT_EQUAL(LANDED, f.final_state);
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "the record is gone");

    /* A restart where it lies -- in a valley 60 m above the pad it left, in a
     * wind -- is not a flight. */
    harness_restart(RESET_POWER_EVENT);
    uint32_t t = mock_time_ms + 1;
    mock_pressure.pressure_pa = mp_pad_pa(&COLD) - 700.0f;
    run_to_pad(&t);
    TEST_ASSERT_EQUAL_STRING("not resumed: no record", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
}

/* A flight scrubbed on the pad leaves its record. The bench it comes back to
 * is where it ends, so a later start in a lift or on a hill road has nothing
 * to resume against. */
void test_FLT_BROWN_08_a_record_left_by_a_scrubbed_flight_is_cleared_on_the_bench(void) {
    start(70, true, (float)PAD_PA);
    harness_usb(true);
    run_to_pad(&now);
    wait_ms(1000);
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "the record is gone");
    uint32_t writes = mock_fs_write_count;
    wait_ms(30000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, mock_fs_write_count, "and cleared once only");

    harness_restart(RESET_POWER_EVENT);
    uint32_t t = mock_time_ms + 1;
    for (uint32_t end = t + 20000u; t < end; t++) {
        mock_pressure.pressure_pa = (float)PAD_PA - 1200.0f + 0.07f * (float)(end - t); /* rising 6 m/s, 100 m up */
        tick(t);
    }
    TEST_ASSERT_EQUAL_STRING("not resumed: no record", flight_resume_text(&ctx));
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
}

void test_FLT_BROWN_08_a_bench_with_no_record_writes_nothing(void) {
    start(71, false, (float)PAD_PA);
    harness_usb(true);
    run_to_pad(&now);
    uint32_t writes = mock_fs_write_count;
    wait_ms(30000);
    TEST_ASSERT_EQUAL_UINT32(writes, mock_fs_write_count);
}

/* The record is held while the flight is in the air, whatever is stored. */
void test_FLT_BROWN_01_the_record_lasts_the_whole_flight(void) {
    flight_conditions_t c = {.pad_s = 12.0f, .stop_after_apogee = true};
    fly(&ROCKETS[SUBSONIC].r, &COLD, &c, 61, 200.0f);
    TEST_ASSERT_TRUE(pad_record_stored());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_BROWN_02_above_the_ground_and_moving_is_a_flight);
    RUN_TEST(test_FLT_BROWN_03_a_still_board_is_not_airborne);
    RUN_TEST(test_FLT_BROWN_01_a_damaged_record_is_no_record);
    RUN_TEST(test_FLT_BROWN_05_no_record_is_reported);
    RUN_TEST(test_FLT_BROWN_05_at_ground_level_is_reported);
    RUN_TEST(test_FLT_BROWN_05_on_usb_is_reported);
    RUN_TEST(test_FLT_BROWN_05_no_sample_in_time_is_reported);
    RUN_TEST(test_FLT_BROWN_03_a_still_board_at_altitude_goes_to_the_pad);
    RUN_TEST(test_FLT_BROWN_02_two_bad_readings_cannot_decide_it);
    RUN_TEST(test_FLT_BROWN_02_every_cause_of_restart_resumes_the_flight);
    RUN_TEST(test_FLT_BROWN_06_a_descent_resumed_fires_every_channel_afresh);
    RUN_TEST(test_FLT_BROWN_06_a_resume_below_the_main_height_fires_both);
    RUN_TEST(test_FLT_BROWN_06_a_delay_counts_in_full_from_the_resume);
    RUN_TEST(test_FLT_BROWN_06_a_climb_resumed_never_fires_before_apogee);
    RUN_TEST(test_FLT_BROWN_06_the_emergency_fire_applies_from_the_resume);
    RUN_TEST(test_FLT_BROWN_04_a_landing_clears_the_resume_state);
    RUN_TEST(test_FLT_BROWN_08_a_record_left_by_a_scrubbed_flight_is_cleared_on_the_bench);
    RUN_TEST(test_FLT_BROWN_08_a_bench_with_no_record_writes_nothing);
    RUN_TEST(test_FLT_BROWN_01_the_record_lasts_the_whole_flight);
    return UNITY_END();
}
