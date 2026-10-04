/*
 * Ground test mode through the flight software [GND-TEST-05..13, DD-087]:
 * a board started with its switch closed, the procedure run from the
 * opening, and what the mode must never do.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "board_harness.h"
#include "../src/atmosphere.h"
#include "../src/ground_test_seq.h"
#include "../src/hal.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f

static const char BOTH[] = "[pyro]\npyro1_mode=delay\npyro2_mode=agl\npyro2_value=300\n";
static const char ONLY_1[] = "[pyro]\npyro1_mode=delay\npyro2_mode=none\n";
static const char ONLY_2[] = "[pyro]\npyro1_mode=none\npyro2_mode=agl\npyro2_value=300\n";
static const char NEITHER[] = "[pyro]\npyro1_mode=none\npyro2_mode=none\n";

static uint32_t t;
static bool saw[STATE_COUNT];

static void run_for(uint32_t ms) {
    for (uint32_t end = t + ms; t < end;) {
        tick(++t);
        saw[ctx.current_state] = true;
    }
}

static void power_up(bool switch_closed, const char *config) {
    boot_like_hardware(1);
    harness_config(config);
    mock_ground_test_pin = switch_closed;
    memset(saw, 0, sizeof(saw));
    t = 0;
    tick(t);
}

/* In the mode, held closed long enough to arm, then opened. Returns when. */
static uint32_t enter_and_open(const char *config) {
    power_up(true, config);
    run_for(6000u);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    mock_ground_test_pin = false;
    return t;
}

static bool said_last(gt_sound_t sound) {
    return harness_gt_sound_n > 0 && harness_gt_sounds[harness_gt_sound_n - 1] == sound;
}

/* ── Entering the mode [GND-TEST-05, GND-TEST-06] ─────────────────── */

void test_GND_TEST_05_started_with_the_switch_closed_enters_ground_test(void) {
    power_up(true, BOTH);
    run_for(20000u);
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    TEST_ASSERT_TRUE_MESSAGE(saw[BOOT_SENSOR] && saw[BOOT_CONTINUITY], "after the sensor and pyro checks");
    TEST_ASSERT_FALSE(saw[BOOT_CALIBRATE]);
    TEST_ASSERT_FALSE(saw[PAD_IDLE]);
    TEST_ASSERT_EQUAL_MESSAGE(GT_SOUND_ALERT, harness_gt_sounds[0], "the mode is announced [GND-TEST-06]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, harness_spec_plays, "and the pad verdict is not");
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, "!GT MODE"), "progress goes out as diagnostic lines [TEL-05]");
    TEST_ASSERT_NULL(strstr(mock_uart_buf, "$PYRO"));
}

void test_GND_TEST_05_started_with_the_switch_open_goes_to_the_pad(void) {
    power_up(false, BOTH);
    run_for(20000u);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_EQUAL_INT(0, harness_gt_sound_n);
}

void test_GND_TEST_05_closed_only_briefly_goes_to_the_pad(void) {
    power_up(true, BOTH);
    run_for(1000u);
    mock_ground_test_pin = false;
    run_for(20000u);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
}

/* A board resuming a flight carries on flying, whatever the switch says:
 * here it restarts descending at 10 m/s through 300 m. */
void test_GND_TEST_05_a_resumed_flight_outranks_the_switch(void) {
    boot_like_hardware(1);
    write_pad_record((int32_t)PAD_PA);
    mock_ground_test_pin = true;
    memset(saw, 0, sizeof(saw));
    for (t = 0; t < 6000u; t++) {
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, 300.0f - 0.010f * (float)t);
        tick(t);
        saw[ctx.current_state] = true;
    }
    TEST_ASSERT_FALSE(saw[GROUND_TEST]);
    TEST_ASSERT_TRUE(saw[FALLING] || saw[DROGUE_DESCENT] || saw[CHUTE_DESCENT]);
}

void test_GND_TEST_06_usb_does_not_take_the_buzzer(void) {
    enter_and_open(BOTH);
    run_for(1000u);
    harness_usb(true);
    run_for(20000u);
    TEST_ASSERT_EQUAL_INT(0, harness_usb_ok_plays);
    TEST_ASSERT_EQUAL_INT(0, harness_spec_plays);
    TEST_ASSERT_EQUAL_INT(2, mock_pulse_count);
}

/* ── The procedure [GND-TEST-07..10] ──────────────────────────────── */

void test_GND_TEST_07_the_procedure(void) {
    uint32_t opened = enter_and_open(BOTH);
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(2, mock_pulse_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_pulses[0].channel);
    TEST_ASSERT_EQUAL_UINT8(2, mock_pulses[1].channel);
    TEST_ASSERT_UINT32_WITHIN(30u, opened + GT_DEBOUNCE_MS + GT_COUNTDOWN_MS, mock_pulses[0].start_ms);
    TEST_ASSERT_UINT32_WITHIN(30u, mock_pulses[0].start_ms + GT_TONE_MS + GT_COUNTDOWN_MS, mock_pulses[1].start_ms);
    const gt_sound_t want[] = {GT_SOUND_ALERT, GT_SOUND_COUNTDOWN, GT_SOUND_TONE, GT_SOUND_COUNTDOWN,
                               GT_SOUND_ALL_CLEAR};
    TEST_ASSERT_EQUAL_INT(5, harness_gt_sound_n);
    TEST_ASSERT_EQUAL_INT_ARRAY(want, harness_gt_sounds, 5);
    TEST_ASSERT_NOT_NULL(strstr(mock_uart_buf, "!GT FIRE 1 fired"));
    TEST_ASSERT_NOT_NULL(strstr(mock_uart_buf, "!GT FIRE 2 fired"));
}

void test_GND_TEST_08_only_an_enabled_channel_fires(void) {
    enter_and_open(ONLY_2);
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(1, mock_pulse_count);
    TEST_ASSERT_EQUAL_UINT8(2, mock_pulses[0].channel);
    const gt_sound_t one[] = {GT_SOUND_ALERT, GT_SOUND_COUNTDOWN, GT_SOUND_ALL_CLEAR};
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, harness_gt_sound_n, "no tone and no second countdown");
    TEST_ASSERT_EQUAL_INT_ARRAY(one, harness_gt_sounds, 3);

    enter_and_open(ONLY_1);
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(1, mock_pulse_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_pulses[0].channel);
    TEST_ASSERT_EQUAL_INT(3, harness_gt_sound_n);

    enter_and_open(NEITHER);
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_TRUE_MESSAGE(said_last(GT_SOUND_ALL_CLEAR), "the countdown leads to the all-clear");
}

/* The pin assignment decides what is a pyro channel [PYR-HEALTH-02]. */
void test_GND_TEST_08_a_channel_given_to_the_script_does_not_fire(void) {
    boot_like_hardware(1);
    harness_config(BOTH);
    harness_give_to_script(1);
    mock_ground_test_pin = true;
    t = 0;
    tick(t);
    run_for(6000u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(1, mock_pulse_count);
    TEST_ASSERT_EQUAL_UINT8(2, mock_pulses[0].channel);
}

/* Opened before the mode has been announced for a second: not an opening. */
void test_GND_TEST_09_an_opening_counts_only_after_a_second_held(void) {
    power_up(true, BOTH);
    for (t = 0; ctx.current_state != GROUND_TEST && t < 20000u;)
        tick(++t);
    run_for(600u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_TRUE(said_last(GT_SOUND_ALERT));
}

/* A bounce shorter than 100 ms is not a change of the switch. */
void test_GND_TEST_09_a_bounce_is_not_an_opening(void) {
    power_up(true, BOTH);
    run_for(6000u);
    for (int i = 0; i < 20; i++) {
        mock_ground_test_pin = false;
        run_for(60u);
        mock_ground_test_pin = true;
        run_for(200u);
    }
    run_for(10000u);
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, harness_gt_sound_n, "nothing but the announcement");
}

void test_GND_TEST_10_closing_the_switch_stops_the_procedure(void) {
    /* In the first countdown, in the tone, and in the second countdown. */
    const uint32_t closed_after[] = {2000u, GT_COUNTDOWN_MS + 1500u, GT_COUNTDOWN_MS + GT_TONE_MS + 2500u};
    const int fired_by_then[] = {0, 1, 1};
    for (unsigned k = 0; k < 3; k++) {
        enter_and_open(BOTH);
        run_for(closed_after[k]);
        mock_ground_test_pin = true;
        run_for(30000u);
        TEST_ASSERT_EQUAL_INT(fired_by_then[k], mock_pulse_count);
        TEST_ASSERT_TRUE_MESSAGE(said_last(GT_SOUND_ALERT), "the mode is announced again");
        TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    }
}

/* ── What the mode never does [GND-TEST-11] ───────────────────────── */

void test_GND_TEST_11_never_flies(void) {
    power_up(true, BOTH);
    run_for(6000u);
    for (int i = 0; i < 20000; i++) {
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, (float)i * 0.05f);
        run_for(1u);
    }
    TEST_ASSERT_EQUAL(GROUND_TEST, ctx.current_state);
    TEST_ASSERT_FALSE(saw[ASCENT]);
    TEST_ASSERT_FALSE(hal_log_active());
    TEST_ASSERT_EQUAL_INT(0, mock_pulse_count);
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "and leaves nothing to resume against");
}

void test_GND_TEST_11_nothing_follows_the_all_clear(void) {
    enter_and_open(BOTH);
    run_for(30000u);
    int sounds = harness_gt_sound_n;
    mock_ground_test_pin = true;
    run_for(5000u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(2, mock_pulse_count);
    TEST_ASSERT_EQUAL_INT(sounds, harness_gt_sound_n);
}

/* ── Delivered on command [GND-TEST-13] ───────────────────────────── */

void test_GND_TEST_13_no_health_reading_withholds_a_fire(void) {
    power_up(true, BOTH);
    mock_pyro.p1_good = mock_pyro.p2_good = false;
    mock_pyro.p1_open = mock_pyro.p2_open = true;
    run_for(6000u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(2, mock_pulse_count);
    TEST_ASSERT_TRUE(mock_pulses[0].energised && mock_pulses[1].energised);
}

/* A board whose protection ends the pulse is still commanded, and the
 * procedure runs on to the second channel and says what it saw. */
void test_GND_TEST_13_a_pulse_that_energises_nothing_is_reported_not_refused(void) {
    power_up(true, BOTH);
    mock_pyro.energises_nothing = true;
    run_for(6000u);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(2, mock_pulse_count);
    TEST_ASSERT_NOT_NULL(strstr(mock_uart_buf, "!GT FIRE 1 fault"));
    TEST_ASSERT_NOT_NULL(strstr(mock_uart_buf, "!GT FIRE 2 fault"));
    TEST_ASSERT_TRUE(said_last(GT_SOUND_ALL_CLEAR));
}

/* [CFG-10] The mode runs on the configuration the board started with. */
void test_CFG_10_a_change_saved_in_the_mode_waits_for_the_next_start(void) {
    power_up(true, ONLY_1);
    run_for(6000u);
    harness_config(BOTH);
    mock_ground_test_pin = false;
    run_for(30000u);
    TEST_ASSERT_EQUAL_INT(1, mock_pulse_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_pulses[0].channel);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_GND_TEST_05_started_with_the_switch_closed_enters_ground_test);
    RUN_TEST(test_GND_TEST_05_started_with_the_switch_open_goes_to_the_pad);
    RUN_TEST(test_GND_TEST_05_closed_only_briefly_goes_to_the_pad);
    RUN_TEST(test_GND_TEST_05_a_resumed_flight_outranks_the_switch);
    RUN_TEST(test_GND_TEST_06_usb_does_not_take_the_buzzer);
    RUN_TEST(test_GND_TEST_07_the_procedure);
    RUN_TEST(test_GND_TEST_08_only_an_enabled_channel_fires);
    RUN_TEST(test_GND_TEST_08_a_channel_given_to_the_script_does_not_fire);
    RUN_TEST(test_GND_TEST_09_an_opening_counts_only_after_a_second_held);
    RUN_TEST(test_GND_TEST_09_a_bounce_is_not_an_opening);
    RUN_TEST(test_GND_TEST_10_closing_the_switch_stops_the_procedure);
    RUN_TEST(test_GND_TEST_11_never_flies);
    RUN_TEST(test_GND_TEST_11_nothing_follows_the_all_clear);
    RUN_TEST(test_GND_TEST_13_no_health_reading_withholds_a_fire);
    RUN_TEST(test_GND_TEST_13_a_pulse_that_energises_nothing_is_reported_not_refused);
    RUN_TEST(test_CFG_10_a_change_saved_in_the_mode_waits_for_the_next_start);
    return UNITY_END();
}
