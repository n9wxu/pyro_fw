/*
 * Buzzer pattern player unit tests — v2 async task architecture.
 *
 * Tests the full encode-then-play pipeline by driving hal_tasks_tick()
 * while advancing mock_time_ms.  All tone_on/off calls are counted via
 * mock_buzzer_tone_on/off_count from hal_test.c.
 *
 * Test IDs:
 *   BUZ-PAT-01  BEEP_ALL_GOOD (1,1)  — chirp count + digit beeps
 *   BUZ-PAT-02  BEEP_P1_FAULT (2,3)  — multi-beep digits
 *   BUZ-PAT-03  altitude 165          — digit decomposition 1-6-5
 *   BUZ-ACT-01  is_active() lifecycle — idle→active→idle
 *   BUZ-ACT-02  buzzer_stop()         — immediate silence
 *   BUZ-ACT-03  repeat flag           — pattern restarts at sentinel
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include "../src/buzzer.h"
#include "../src/hal.h"
#include <stdint.h>
#include <stdbool.h>

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
    /* Before play: inactive */
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "Buzzer should be inactive before play");

    buzzer_play_code(BEEP_ALL_GOOD, false);

    /* After request: active (encoding pending) */
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "Buzzer should be active after play_code");

    /* Drive to completion — BEEP_ALL_GOOD non-repeating must end */
    /* Max possible: 10 chirps(600ms) + pause(500) + 1 beep(100) + gap(300)
     *               + 1 beep(100) + code_gap(500) = 2100ms; add margin */
    uint32_t elapsed = drive_until_idle(5000);

    TEST_ASSERT_TRUE_MESSAGE(elapsed < 5000, "Buzzer never became idle — possible infinite loop");
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "Buzzer should be inactive after pattern completes");
}

/* ── BUZ-ACT-02: buzzer_stop() immediately silences ──────────────── */

void test_BUZ_ACT_02_stop(void) {
    buzzer_play_code(BEEP_ALL_GOOD, true); /* repeating */
    TEST_ASSERT_TRUE(buzzer_is_active());

    /* Let it run a few ticks so it's in BZ_PLAYING */
    drive_for(100);

    /* Stop should silence immediately */
    buzzer_stop();
    TEST_ASSERT_FALSE_MESSAGE(buzzer_is_active(), "buzzer_stop() did not deactivate buzzer");

    /* Further ticks should produce no more tone events */
    int on_before = mock_buzzer_tone_on_count;
    int off_before = mock_buzzer_tone_off_count;
    drive_for(500);
    TEST_ASSERT_EQUAL_MESSAGE(on_before, mock_buzzer_tone_on_count, "tone_on called after buzzer_stop()");
    /* tone_off is allowed once (the stop itself) but not more */
    TEST_ASSERT_EQUAL_MESSAGE(off_before, mock_buzzer_tone_off_count, "extra tone_off after buzzer_stop()");
}

/* ── BUZ-PAT-01: BEEP_ALL_GOOD (1,1) tone count ──────────────────── */
/* Expected tone_on events:
 *   10 chirps = 10 on events
 *   digit1=1  = 1 on event
 *   digit2=1  = 1 on event
 *   Total: 12 on events */

void test_BUZ_PAT_01_all_good_tone_count(void) {
    /* The initial buzzer_play_code call emits one tone_off */
    mock_buzzer_tone_off_count = 0;
    mock_buzzer_tone_on_count = 0;

    buzzer_play_code(BEEP_ALL_GOOD, false);
    drive_until_idle(5000);

    /* 10 chirps on + digit1(1 beep) on + digit2(1 beep) on = 12 */
    TEST_ASSERT_EQUAL_MESSAGE(12, mock_buzzer_tone_on_count,
                              "BEEP_ALL_GOOD should produce 12 tone_on events (10 chirps + 1 + 1)");
}

/* ── BUZ-PAT-02: BEEP_P1_FAULT (2,3) digit beep counts ──────────── */
/* Expected tone_on events:
 *   10 chirps    = 10 on
 *   digit1=2     = 2 on
 *   digit2=3     = 3 on
 *   Total: 15 on events */

void test_BUZ_PAT_02_fault_code_digit_counts(void) {
    mock_buzzer_tone_on_count = 0;
    mock_buzzer_tone_off_count = 0;

    buzzer_play_code(BEEP_P1_FAULT, false);
    drive_until_idle(6000);

    TEST_ASSERT_EQUAL_MESSAGE(15, mock_buzzer_tone_on_count,
                              "BEEP_P1_FAULT (2,3) should produce 15 tone_on events (10+2+3)");
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

    /* Drive until the long pause + long beep + short pause + all digits
     * have been emitted.  Total time before loop sentinel:
     *   long_pause(2000) + long_beep(500) + short_pause(300)
     *   + digit1: 1*(on100+gap400) = 500
     *   + digit6: 5*(on100+gap200)+on100+gap400 = 5*300+100+400 = 2000
     *   + digit5: 4*(on100+gap200)+on100+gap400 = 4*300+100+400 = 1700
     *   Total: ~7000ms
     * Add a small margin and drive to just before the sentinel loops. */
    drive_for(8000);

    /* Stop before the loop restarts */
    buzzer_stop();

    /* 1 (long header) + 1 (digit 1) + 6 (digit 6) + 5 (digit 5) = 13 */
    TEST_ASSERT_EQUAL_MESSAGE(13, mock_buzzer_tone_on_count, "altitude 165 should produce 13 tone_on events (1+1+6+5)");
}

/* ── BUZ-ACT-03: repeat flag causes restart at sentinel ──────────── */

void test_BUZ_ACT_03_repeat_restarts(void) {
    buzzer_play_code(BEEP_ALL_GOOD, true); /* repeat = true */

    /* Drive for two full code periods (2 × ~2100ms) */
    drive_for(5000);

    /* With repeat, we should still be active after 5 seconds */
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "Repeating buzzer should still be active after 5s");

    /* And tone_on count should be > 12 (more than one pass) */
    TEST_ASSERT_TRUE_MESSAGE(mock_buzzer_tone_on_count > 12,
                             "Repeating pattern should produce >12 tone_on events (more than one pass)");
}

/* ── BUZ-PAT-04: play_code arms the async task immediately ────────── */

void test_BUZ_PAT_04_task_armed_on_play(void) {
    /* Before: no tone events */
    TEST_ASSERT_EQUAL(0, mock_buzzer_tone_on_count);

    buzzer_play_code(BEEP_ALL_GOOD, false);

    /* Single tick at t=0 triggers encode + first play step */
    mock_time_ms = 0;
    hal_tasks_tick(0);

    /* After encode tick: state transitions to BZ_PLAYING, next tick plays step 0.
     * Depending on next_due_ms=0 and the two ticks, we may see tone action.
     * The key assertion: is_active() is true immediately after play_code. */
    TEST_ASSERT_TRUE_MESSAGE(buzzer_is_active(), "Task should be active after buzzer_play_code");
}

/* ── BUZ-PAT-05: p2_no_open (3,4) — cross-check digit values ──────── */
/* BEEP_P2_NO_OPEN = BEEP_CODE(3,4): digit1=3, digit2=4
 * tone_on: 10 chirps + 3 + 4 = 17 */

void test_BUZ_PAT_05_p2_no_open(void) {
    mock_buzzer_tone_on_count = 0;

    buzzer_play_code(BEEP_P2_NO_OPEN, false);
    drive_until_idle(7000);

    TEST_ASSERT_EQUAL_MESSAGE(17, mock_buzzer_tone_on_count,
                              "BEEP_P2_NO_OPEN (3,4) should produce 17 tone_on events (10+3+4)");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_BUZ_ACT_01_lifecycle);
    RUN_TEST(test_BUZ_ACT_02_stop);
    RUN_TEST(test_BUZ_PAT_01_all_good_tone_count);
    RUN_TEST(test_BUZ_PAT_02_fault_code_digit_counts);
    RUN_TEST(test_BUZ_PAT_03_altitude_165_digits);
    RUN_TEST(test_BUZ_ACT_03_repeat_restarts);
    RUN_TEST(test_BUZ_PAT_04_task_armed_on_play);
    RUN_TEST(test_BUZ_PAT_05_p2_no_open);

    return UNITY_END();
}
