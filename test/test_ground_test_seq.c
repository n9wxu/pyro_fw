/*
 * The ground test procedure's schedule (ground_test_seq.c), stepped once a
 * 20 ms loop against a fake clock [GND-TEST-05..11].
 *
 * Verifies [GND-TEST-07..11, GND-TEST-13].
 */
#include "../src/loop_period.h"
#include "unity.h"
#include "ground_test_seq.h"
#include "ground_test_switch.h"
#include <string.h>

static gt_seq_t s;
static uint32_t now;
static bool pin; /* asserted */

static struct {
    gt_sound_t sound[64];
    uint32_t sound_ms[64];
    int n_sounds;
    uint8_t fire[16];
    uint32_t fire_ms[16];
    int n_fires;
} log_;

/* How the board answers a fire: normally it energises. */
static gt_fire_t answer = GT_FIRE_ENERGISED;
static int busy_left;

static void loop_for(uint32_t ms) {
    uint32_t end = now + ms;
    while (now < end) {
        now += LOOP_PERIOD_MS;
        gt_action_t a = gt_seq_step(&s, pin, now);
        if (a.sound != GT_SOUND_NONE && log_.n_sounds < 64) {
            log_.sound[log_.n_sounds] = a.sound;
            log_.sound_ms[log_.n_sounds++] = now;
        }
        if (a.fire) {
            gt_fire_t r = busy_left > 0 ? (busy_left--, GT_FIRE_BUSY) : answer;
            if (r != GT_FIRE_BUSY && log_.n_fires < 16) {
                log_.fire[log_.n_fires] = a.fire;
                log_.fire_ms[log_.n_fires++] = now;
            }
            gt_seq_fired(&s, a.fire, r, now);
        }
    }
}

static int sounds_of(gt_sound_t k) {
    int n = 0;
    for (int i = 0; i < log_.n_sounds; i++)
        n += log_.sound[i] == k;
    return n;
}

static uint32_t first_sound_ms(gt_sound_t k) {
    for (int i = 0; i < log_.n_sounds; i++)
        if (log_.sound[i] == k)
            return log_.sound_ms[i];
    return 0;
}

/* Powered up asserted, held past the arming time, then released at the
 * returned time. */
static uint32_t begin_and_release(bool p1, bool p2) {
    pin = true;
    gt_seq_begin(&s, p1, p2, true, now);
    loop_for(GT_ARM_MS + 200u);
    pin = false;
    return now;
}

void setUp(void) {
    memset(&s, 0, sizeof(s));
    memset(&log_, 0, sizeof(log_));
    now = 100000u;
    pin = false;
    answer = GT_FIRE_ENERGISED;
    busy_left = 0;
}

void tearDown(void) {}

/* [GND-TEST-06] Ground test mode announces itself, and keeps announcing. */
void test_GT_mode_is_announced(void) {
    pin = true;
    gt_seq_begin(&s, true, true, true, now);
    loop_for(10000u);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
    TEST_ASSERT_EQUAL(1, sounds_of(GT_SOUND_ALERT));
    TEST_ASSERT_EQUAL(0, log_.n_fires);
}

/* [GND-TEST-07] The release starts the countdown, and at zero channel 1
 * fires; then the steady tone for 3 s and another countdown; at its zero
 * channel 2; then the all-clear, then silence. */
void test_GT_both_channels(void) {
    uint32_t released = begin_and_release(true, true);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(2, log_.n_fires);
    TEST_ASSERT_EQUAL(1, log_.fire[0]);
    TEST_ASSERT_EQUAL(2, log_.fire[1]);
    /* The release counts once it has held GT_DEBOUNCE_MS. */
    uint32_t counted = released + GT_DEBOUNCE_MS;
    TEST_ASSERT_UINT32_WITHIN(LOOP_PERIOD_MS, counted, first_sound_ms(GT_SOUND_COUNTDOWN));
    TEST_ASSERT_UINT32_WITHIN(2u * LOOP_PERIOD_MS, counted + GT_COUNTDOWN_MS, log_.fire_ms[0]);
    TEST_ASSERT_UINT32_WITHIN(LOOP_PERIOD_MS, log_.fire_ms[0], first_sound_ms(GT_SOUND_TONE));
    TEST_ASSERT_EQUAL(2, sounds_of(GT_SOUND_COUNTDOWN));
    TEST_ASSERT_UINT32_WITHIN(3u * LOOP_PERIOD_MS, log_.fire_ms[0] + GT_TONE_MS + GT_COUNTDOWN_MS, log_.fire_ms[1]);
    TEST_ASSERT_UINT32_WITHIN(LOOP_PERIOD_MS, log_.fire_ms[1], first_sound_ms(GT_SOUND_ALL_CLEAR));
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
    TEST_ASSERT_EQUAL(1, sounds_of(GT_SOUND_ALL_CLEAR));
}

/* A count a second, from five: zero, and the fire, five seconds on. */
void test_GT_the_countdown_is_a_second_a_count(void) {
    TEST_ASSERT_EQUAL_UINT32(1000u, GT_COUNT_MS);
    TEST_ASSERT_EQUAL_UINT32(5u * 1000u, GT_COUNTDOWN_MS);
    TEST_ASSERT_EQUAL_UINT32(3000u, GT_TONE_MS);
}

/* [GND-TEST-08] One channel enabled: no tone, the all-clear right after it. */
void test_GT_only_channel_1(void) {
    begin_and_release(true, false);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    TEST_ASSERT_EQUAL(1, log_.fire[0]);
    TEST_ASSERT_EQUAL(0, sounds_of(GT_SOUND_TONE));
    TEST_ASSERT_EQUAL(1, sounds_of(GT_SOUND_COUNTDOWN));
    TEST_ASSERT_UINT32_WITHIN(LOOP_PERIOD_MS, log_.fire_ms[0], first_sound_ms(GT_SOUND_ALL_CLEAR));
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
}

void test_GT_only_channel_2(void) {
    uint32_t released = begin_and_release(false, true);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    TEST_ASSERT_EQUAL(2, log_.fire[0]);
    TEST_ASSERT_UINT32_WITHIN(2u * LOOP_PERIOD_MS, released + GT_DEBOUNCE_MS + GT_COUNTDOWN_MS, log_.fire_ms[0]);
    TEST_ASSERT_EQUAL(0, sounds_of(GT_SOUND_TONE));
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
}

/* [GND-TEST-08] No channel enabled: the countdown, then the all-clear. */
void test_GT_no_channel(void) {
    uint32_t released = begin_and_release(false, false);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(0, log_.n_fires);
    TEST_ASSERT_EQUAL(0, sounds_of(GT_SOUND_TONE));
    TEST_ASSERT_UINT32_WITHIN(2u * LOOP_PERIOD_MS, released + GT_DEBOUNCE_MS + GT_COUNTDOWN_MS,
                              first_sound_ms(GT_SOUND_ALL_CLEAR));
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
}

/* [GND-TEST-09] A release counts only after the pin has been held in ground
 * test mode for GT_ARM_MS: released during boot, before the mode was heard,
 * nothing follows until it is put back and pulled again. */
void test_GT_a_release_before_arming_does_nothing(void) {
    pin = false;
    gt_seq_begin(&s, true, true, false, now);
    loop_for(20000u);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
    pin = true;
    loop_for(GT_ARM_MS / 2u);
    pin = false;
    loop_for(20000u);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
    TEST_ASSERT_EQUAL(0, log_.n_fires);
    pin = true;
    loop_for(GT_ARM_MS + 100u);
    pin = false;
    loop_for(30000u);
    TEST_ASSERT_EQUAL(2, log_.n_fires);
}

/* [GND-TEST-09] A bounce shorter than the debounce is not a release. */
void test_GT_a_bounce_is_not_a_release(void) {
    pin = true;
    gt_seq_begin(&s, true, true, true, now);
    loop_for(GT_ARM_MS + 200u);
    pin = false;
    loop_for(GT_DEBOUNCE_MS - 2u * LOOP_PERIOD_MS);
    pin = true;
    loop_for(20000u);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
    TEST_ASSERT_EQUAL(0, sounds_of(GT_SOUND_COUNTDOWN));
}

/* [GND-TEST-10] The pin put back during the countdown aborts it: nothing
 * fires, the mode is announced again, and the next release starts the whole
 * countdown over. */
void test_GT_reasserting_aborts_the_countdown(void) {
    begin_and_release(true, true);
    loop_for(GT_COUNTDOWN_MS / 2u);
    pin = true;
    loop_for(3000u);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
    TEST_ASSERT_EQUAL(0, log_.n_fires);
    TEST_ASSERT_EQUAL(2, sounds_of(GT_SOUND_ALERT));
    pin = false;
    uint32_t released = now;
    loop_for(30000u);
    TEST_ASSERT_EQUAL(2, log_.n_fires);
    TEST_ASSERT_UINT32_WITHIN(2u * LOOP_PERIOD_MS, released + GT_DEBOUNCE_MS + GT_COUNTDOWN_MS, log_.fire_ms[0]);
}

/* [GND-TEST-10] Put back between the channels, in the tone or in the second
 * countdown: channel 2 does not fire. */
void test_GT_reasserting_in_the_tone_stops_the_second(void) {
    begin_and_release(true, true);
    loop_for(GT_DEBOUNCE_MS + GT_COUNTDOWN_MS + GT_TONE_MS / 2u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    pin = true;
    loop_for(GT_TONE_MS + GT_COUNTDOWN_MS + 2000u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
}

void test_GT_reasserting_in_the_second_countdown_stops_the_second(void) {
    begin_and_release(true, true);
    loop_for(GT_DEBOUNCE_MS + GT_COUNTDOWN_MS + GT_TONE_MS + GT_COUNTDOWN_MS / 2u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    TEST_ASSERT_EQUAL(2, sounds_of(GT_SOUND_COUNTDOWN));
    pin = true;
    loop_for(GT_COUNTDOWN_MS + 2000u);
    TEST_ASSERT_EQUAL(1, log_.n_fires);
    TEST_ASSERT_EQUAL(GT_ALERT, s.phase);
}

/* The other channel's pulse still running: the fire is asked again next
 * loop, and the schedule after it starts from the fire. */
void test_GT_a_busy_fire_is_asked_again(void) {
    busy_left = 3;
    begin_and_release(true, true);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(2, log_.n_fires);
    TEST_ASSERT_UINT32_WITHIN(3u * LOOP_PERIOD_MS, log_.fire_ms[0] + GT_TONE_MS + GT_COUNTDOWN_MS, log_.fire_ms[1]);
}

/* [GND-TEST-13] A fire that energised nothing is recorded and the procedure
 * goes on. */
void test_GND_TEST_13_a_fire_that_energised_nothing_is_recorded_and_the_procedure_goes_on(void) {
    answer = GT_FIRE_FAULT;
    begin_and_release(true, true);
    loop_for(30000u);
    TEST_ASSERT_EQUAL(GT_FIRE_FAULT, s.result[0]);
    TEST_ASSERT_EQUAL(GT_FIRE_FAULT, s.result[1]);
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
    TEST_ASSERT_EQUAL(1, sounds_of(GT_SOUND_ALL_CLEAR));
}

/* [GND-TEST-11] Done is final: the pin does nothing more before the next
 * power-up. */
void test_GT_done_is_final(void) {
    begin_and_release(true, true);
    loop_for(30000u);
    int fires = log_.n_fires, sounds = log_.n_sounds;
    for (int i = 0; i < 5; i++) {
        pin = true;
        loop_for(GT_ARM_MS + 500u);
        pin = false;
        loop_for(2u * GT_COUNTDOWN_MS + GT_TONE_MS + 5000u);
    }
    TEST_ASSERT_EQUAL(fires, log_.n_fires);
    TEST_ASSERT_EQUAL(sounds, log_.n_sounds);
    TEST_ASSERT_EQUAL(GT_DONE, s.phase);
}

void test_GT_phase_names(void) {
    TEST_ASSERT_EQUAL_STRING("alert", gt_seq_phase_name(GT_ALERT));
    TEST_ASSERT_EQUAL_STRING("countdown", gt_seq_phase_name(GT_COUNTDOWN));
    TEST_ASSERT_EQUAL_STRING("done", gt_seq_phase_name(GT_DONE));
}


/* ── The switch [GND-TEST-12] ─────────────────────────────────────
 *
 * Across two pads the driven one alternates, and the read one is pulled the
 * other way each time: closed, the read pad follows the drive both ways;
 * open, it follows its pull. A read pad touching ground, or the supply,
 * follows one way only, and is not a closed switch. */

static bool sw_run(gt_switch_t *w, int loops, int (*pad)(bool drive_high, bool pull_up)) {
    bool drive = false, pull = true;
    for (int i = 0; i < loops; i++) {
        gt_switch_out_t o = gt_switch_step(w, pad(drive, pull) != 0);
        drive = o.drive_high;
        pull = o.pull_up;
    }
    return gt_switch_asserted(w);
}
static int closed(bool drive, bool pull) {
    (void)pull;
    return drive;
}
static int open_(bool drive, bool pull) {
    (void)drive;
    return pull;
}
static int stuck_low(bool drive, bool pull) {
    (void)drive;
    (void)pull;
    return 0;
}
static int stuck_high(bool drive, bool pull) {
    (void)drive;
    (void)pull;
    return 1;
}

void test_GT_SW_switch_to_ground(void) {
    gt_switch_t w;
    gt_switch_begin(&w, false);
    TEST_ASSERT_TRUE(gt_switch_step(&w, false).pull_up);
    TEST_ASSERT_TRUE(gt_switch_asserted(&w));
    gt_switch_step(&w, true);
    TEST_ASSERT_FALSE(gt_switch_asserted(&w));
}

void test_GT_SW_two_pads_closed_and_open(void) {
    gt_switch_t w;
    gt_switch_begin(&w, true);
    TEST_ASSERT_TRUE(sw_run(&w, 6, closed));
    TEST_ASSERT_FALSE(sw_run(&w, 6, open_));
    TEST_ASSERT_TRUE(sw_run(&w, 6, closed));
}

void test_GT_SW_two_pads_a_stuck_pad_is_not_closed(void) {
    gt_switch_t w;
    gt_switch_begin(&w, true);
    TEST_ASSERT_FALSE(sw_run(&w, 10, stuck_low));
    TEST_ASSERT_FALSE(sw_run(&w, 10, stuck_high));
}

/* The drive alternates and the pull opposes it, every step. */
void test_GT_SW_two_pads_drive_alternates(void) {
    gt_switch_t w;
    gt_switch_begin(&w, true);
    gt_switch_out_t a = gt_switch_step(&w, false);
    gt_switch_out_t b = gt_switch_step(&w, true);
    TEST_ASSERT_TRUE(a.drive_high != b.drive_high);
    TEST_ASSERT_TRUE(a.pull_up != a.drive_high);
    TEST_ASSERT_TRUE(b.pull_up != b.drive_high);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_GT_mode_is_announced);
    RUN_TEST(test_GT_both_channels);
    RUN_TEST(test_GT_only_channel_1);
    RUN_TEST(test_GT_only_channel_2);
    RUN_TEST(test_GT_no_channel);
    RUN_TEST(test_GT_a_release_before_arming_does_nothing);
    RUN_TEST(test_GT_a_bounce_is_not_a_release);
    RUN_TEST(test_GT_reasserting_aborts_the_countdown);
    RUN_TEST(test_GT_reasserting_in_the_tone_stops_the_second);
    RUN_TEST(test_GT_reasserting_in_the_second_countdown_stops_the_second);
    RUN_TEST(test_GT_the_countdown_is_a_second_a_count);
    RUN_TEST(test_GT_a_busy_fire_is_asked_again);
    RUN_TEST(test_GND_TEST_13_a_fire_that_energised_nothing_is_recorded_and_the_procedure_goes_on);
    RUN_TEST(test_GT_done_is_final);
    RUN_TEST(test_GT_phase_names);
    RUN_TEST(test_GT_SW_switch_to_ground);
    RUN_TEST(test_GT_SW_two_pads_closed_and_open);
    RUN_TEST(test_GT_SW_two_pads_a_stuck_pad_is_not_closed);
    RUN_TEST(test_GT_SW_two_pads_drive_alternates);
    return UNITY_END();
}
