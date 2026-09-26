/*
 * Buzzer pattern player unit tests — v2 async task architecture.
 *
 * Tests the full encode-then-play pipeline by driving hal_tasks_tick()
 * while advancing mock_time_ms.  All tone_on/off calls are counted via
 * mock_buzzer_tone_on/off_count from hal_test.c.
 *
 * Test IDs:
 *   BUZ-PAT-01  chirp (ready to fly)   — one warble, 25 cycles
 *   BUZ-PAT-02  counted codes          — one and two groups
 *   BUZ-PAT-03  altitude 165           — digit decomposition 1-6-5
 *   BUZ-ACT-01  is_active() lifecycle  — idle→active→idle
 *   BUZ-ACT-02  buzzer_stop()          — immediate silence
 *   BUZ-ACT-03  repeat until stopped   — pattern restarts at sentinel
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include "../src/buzzer.h"
#include "../src/beep_store.h"
#include "../src/hal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const beep_spec_t CHIRP = {BK_CHIRP, 0, 0};
static const beep_spec_t TONE = {BK_TONE, 0, 0};
static const beep_spec_t FIVE = {BK_CODE, 5, 0};
static const beep_spec_t TWO_THREE = {BK_CODE, 2, 3};
static const beep_spec_t ONE = {BK_CODE, 1, 0};

/* ── Test helpers ─────────────────────────────────────────────────── */

/*
 * Drive the buzzer task forward in time until it becomes idle or
 * max_ms milliseconds have elapsed.  Returns the number of ms elapsed.
 */
static uint32_t drive_until_idle(uint32_t max_ms) {
    for (uint32_t t = 1; t <= max_ms; t++) {
        mock_time_ms = t;
        hal_tasks_tick(t);
        if (!buzzer_is_active())
            return t;
    }
    return max_ms;
}

/*
 * Drive the task for exactly N milliseconds, calling hal_tasks_tick
 * every ms.
 */
static void drive_for(uint32_t ms) {
    uint32_t start = mock_time_ms;
    for (uint32_t t = start + 1; t <= start + ms; t++) {
        mock_time_ms = t;
        hal_tasks_tick(t);
    }
}

void setUp(void) {
    mock_reset_all();
    buzzer_init(); /* registers the async task */
}

void tearDown(void) {}

/* ── BUZ-ACT-01: is_active() lifecycle ───────────────────────────── */

void test_BUZ_ACT_01_lifecycle(void) {
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "Buzzer should be inactive before play");

    buzzer_play_spec(&ONE, 500, 1); /* play once */
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "Buzzer should be active after play_spec");

    uint32_t elapsed = drive_until_idle(5000);
    TEST_ASSERT_TRUE_MESSAGE(elapsed < 5000, "Buzzer never became idle — possible infinite loop");
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "Buzzer should be inactive after pattern completes");
}

/* ── BUZ-ACT-02: buzzer_stop() immediately silences ──────────────── */

void test_BUZ_ACT_02_stop(void) {
    buzzer_play_spec(&FIVE, 5000, 0); /* until stopped */
    TEST_ASSERT_TRUE(buzzer_is_active());
    drive_for(100);

    buzzer_stop();
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "buzzer_stop() did not deactivate buzzer");

    int on_before = mock_buzzer_tone_on_count;
    int off_before = mock_buzzer_tone_off_count;
    drive_for(500);
    TEST_ASSERT_EQUAL_MESSAGE(on_before, mock_buzzer_tone_on_count, "tone_on called after buzzer_stop()");
    TEST_ASSERT_EQUAL_MESSAGE(off_before, mock_buzzer_tone_off_count, "extra tone_off after buzzer_stop()");
}

/* ── BUZ-PAT-01: the ready-to-fly chirp is a warble, not a count ─── */

void test_BUZ_PAT_01_chirp_is_one_warble(void) {
    buzzer_play_spec(&CHIRP, 0, 1);
    drive_until_idle(5000);
    TEST_ASSERT_EQUAL_MESSAGE(25, mock_buzzer_tone_on_count, "one pass of the chirp is 25 short tones");
}

/* ── BUZ-PAT-02: counted codes carry exactly their counts ─────────── */

void test_BUZ_PAT_02_counted_codes(void) {
    buzzer_play_spec(&FIVE, 0, 1);
    drive_until_idle(5000);
    TEST_ASSERT_EQUAL_MESSAGE(5, mock_buzzer_tone_on_count, "a 5 is five beeps and nothing in front of them");

    mock_buzzer_tone_on_count = 0;
    buzzer_play_spec(&TWO_THREE, 0, 1);
    drive_until_idle(10000);
    TEST_ASSERT_EQUAL_MESSAGE(5, mock_buzzer_tone_on_count, "a 2-3 is two beeps then three");
}

/* ── BUZ-PAT-03: altitude 165 digit decomposition ────────────────── */
/* Digits of 165: 1, 6, 5
 * Expected tone_on events:
 *   header long beep = 1 on
 *   digit 1 (=1)     = 1 on
 *   digit 6 (=6)     = 6 on
 *   digit 5 (=5)     = 5 on
 *   Total: 13 on events before first loop-back
 * Altitude always loops — we stop after one pass by stopping. */

void test_BUZ_PAT_03_altitude_165_digits(void) {
    mock_buzzer_tone_on_count = 0;
    mock_buzzer_tone_off_count = 0;

    buzzer_play_altitude(165);

    /* long_pause(2000) + long_beep(500) + short_pause(300)
     *   + digit1: 1*(on100+gap400) = 500
     *   + digit6: 5*(on100+gap200)+on100+gap400 = 2000
     *   + digit5: 4*(on100+gap200)+on100+gap400 = 1700
     *   Total: ~7000ms */
    drive_for(8000);
    buzzer_stop();

    /* 1 (long header) + 1 (digit 1) + 6 (digit 6) + 5 (digit 5) = 13 */
    TEST_ASSERT_EQUAL_MESSAGE(13, mock_buzzer_tone_on_count, "altitude 165 should produce 13 tone_on events (1+1+6+5)");
}

/* ── BUZ-ACT-03: repeat 0 restarts at the sentinel until stopped ─── */

void test_BUZ_ACT_03_repeat_restarts(void) {
    buzzer_play_spec(&ONE, 500, 0);
    drive_for(3000); /* one beep plus a 500 ms gap is 600 ms a pass */
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "a repeat of 0 plays until stopped");
    TEST_ASSERT_TRUE_MESSAGE(mock_buzzer_tone_on_count >= 4, "and must have started more than one pass");
}

/* ── BUZ-PAT-04: play arms the async task immediately ─────────────── */

void test_BUZ_PAT_04_task_armed_on_play(void) {
    TEST_ASSERT_EQUAL(0, mock_buzzer_tone_on_count);
    buzzer_play_spec(&ONE, 0, 1);
    mock_time_ms = 0;
    hal_tasks_tick(0);
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "Task should be active after buzzer_play_spec");
}

/* ── BUZ-PAT-06: a repeat of 2 plays exactly twice [BUZ-02] ──────── */

void test_BUZ_PAT_06_repeat_count_2_buz02(void) {
    buzzer_play_spec(&FIVE, 1000, 2);
    uint32_t elapsed = drive_until_idle(10000);
    TEST_ASSERT_TRUE_MESSAGE(elapsed < 10000, "a repeat of 2 must stop");
    TEST_ASSERT_EQUAL_MESSAGE(10, mock_buzzer_tone_on_count, "two passes of a 5 are ten beeps");
}

/* ── BUZ-PAT-07: the personality's gap separates the passes ───────── */

void test_BUZ_PAT_07_gap_between_passes(void) {
    buzzer_play_spec(&ONE, 2000, 0);
    drive_for(1500); /* one 100 ms beep, then inside the 2 s gap */
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_buzzer_tone_on_count, "the second pass must wait out the gap");
    drive_for(1000);
    TEST_ASSERT_EQUAL_MESSAGE(2, mock_buzzer_tone_on_count, "and start once it has");
    buzzer_stop();
}

/* ── BUZ-PAT-08: altitude 1000 — zero digits need 10 beeps each ──── */

void test_BUZ_PAT_08_altitude_1000_zero_digits(void) {
    buzzer_play_altitude(1000);
    /* 2000(pause) + 500(beep) + 300(pause) + digit 1: 500
     *   + digit 0: 10*(100+200)-200+400 = 3200ms  (×3)  ≈ 12900 ms */
    drive_for(12800);
    buzzer_stop();
    /* 1 (header long beep) + 1 + 10 + 10 + 10 = 32 */
    TEST_ASSERT_EQUAL_MESSAGE(32, mock_buzzer_tone_on_count,
                              "altitude 1000 should produce 32 tone_on events (1+1+10+10+10)");
}

/* ── BUZ-PAT-09: altitude 10000 — five digits with four zeros ─────── */

void test_BUZ_PAT_09_altitude_10000(void) {
    buzzer_play_altitude(10000);
    drive_for(16000);
    buzzer_stop();
    TEST_ASSERT_EQUAL_MESSAGE(42, mock_buzzer_tone_on_count,
                              "altitude 10000 should produce 42 tone_on events (1+1+10+10+10+10)");
}

/* ── OK on USB [USB-03] ───────────────────────────────────────────
 *
 * Two short bursts, once. Nothing like the ready-to-fly warble, and it must
 * not repeat: a board on USB says nothing else. */
void test_BUZ_PAT_10_usb_ok_is_one_double_chirp(void) {
    buzzer_play_usb_ok();
    uint32_t elapsed = drive_until_idle(5000);
    TEST_ASSERT_TRUE_MESSAGE(elapsed < 1500, "the double chirp must be short and stop");
    TEST_ASSERT_EQUAL_MESSAGE(10, mock_buzzer_tone_on_count, "two bursts of five chirps");
}

/* ── A new outcome can interrupt a tone [REV-04] ──────────────────
 *
 * The pad check re-announces when its answer changes, which can land in the
 * middle of a step that holds the tone on. */
void test_BUZ_ACT_04_new_outcome_silences_the_old_one(void) {
    buzzer_play_spec(&TONE, 0, 0);
    drive_for(100);
    TEST_ASSERT_EQUAL(1, mock_buzzer_tone_on_count);
    int off_before = mock_buzzer_tone_off_count;
    buzzer_play_spec(&FIVE, 5000, 0);
    TEST_ASSERT_EQUAL_MESSAGE(off_before + 1, mock_buzzer_tone_off_count,
                              "the tone must stop when the outcome changes");
}

/* ── Beep store errors say what went wrong [BUZ-CODE-10, REV-22] ─── */

void test_BEEP_STORE_01_write_failure_is_not_a_digit_error(void) {
    /* Fill every slot of the in-memory filesystem so beep.ini cannot be
     * created. */
    hal_fs_write_file("a.txt", "a", 1);
    hal_fs_write_file("b.txt", "b", 1);
    hal_fs_write_file("c.txt", "c", 1);
    hal_fs_write_file("d.txt", "d", 1);

    beep_table_t t;
    beep_codes_defaults(&t);
    beep_verdict_t v = beep_store_save(&t);
    TEST_ASSERT_EQUAL_MESSAGE(BEEP_ERR_STORE, v.err, "a flash write failure is not a beep-count error");
    TEST_ASSERT_EQUAL_STRING(beep_codes_strerror(BEEP_ERR_STORE), v.what);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(beep_codes_strerror(v.err), beep_codes_strerror(BEEP_ERR_DIGIT_RANGE)));
}

void test_BEEP_STORE_02_missing_file_reason_reaches_the_caller(void) {
    char reason[96] = "unchanged";
    beep_store_load(reason, (int)sizeof(reason));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reason, "no beep.ini"), reason);
    TEST_ASSERT_EQUAL_STRING(beep_store_reason(), reason);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_BUZ_ACT_01_lifecycle);
    RUN_TEST(test_BUZ_ACT_02_stop);
    RUN_TEST(test_BUZ_PAT_01_chirp_is_one_warble);
    RUN_TEST(test_BUZ_PAT_02_counted_codes);
    RUN_TEST(test_BUZ_PAT_03_altitude_165_digits);
    RUN_TEST(test_BUZ_ACT_03_repeat_restarts);
    RUN_TEST(test_BUZ_PAT_04_task_armed_on_play);
    RUN_TEST(test_BUZ_PAT_06_repeat_count_2_buz02);
    RUN_TEST(test_BUZ_PAT_07_gap_between_passes);
    RUN_TEST(test_BUZ_PAT_08_altitude_1000_zero_digits);
    RUN_TEST(test_BUZ_PAT_09_altitude_10000);
    RUN_TEST(test_BUZ_ACT_04_new_outcome_silences_the_old_one);
    RUN_TEST(test_BUZ_PAT_10_usb_ok_is_one_double_chirp);
    RUN_TEST(test_BEEP_STORE_01_write_failure_is_not_a_digit_error);
    RUN_TEST(test_BEEP_STORE_02_missing_file_reason_reaches_the_caller);

    return UNITY_END();
}
