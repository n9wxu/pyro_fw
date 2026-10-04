/*
 * The altitude beep-out [BUZ-04], heard from the buzzer pin: buzzer.c
 * run against a HAL that records each tone edge, and the digits read back
 * the way a flier counts them -- a long beep, then each digit's beeps, ten for
 * a zero.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "buzzer.h"
#include "async_task.h"
#include <stdint.h>
#include <string.h>

static async_task_t *task;
static uint32_t now;
static bool tone;

#define EDGES 2048
static uint32_t on_at[EDGES], off_at[EDGES];
static int beeps;

void hal_buzzer_init(void) {}
void hal_buzzer_task_register(async_task_t *t) {
    task = t;
}
void hal_buzzer_tone_on(void) {
    if (!tone && beeps < EDGES)
        on_at[beeps] = now;
    tone = true;
}
void hal_buzzer_tone_off(void) {
    if (tone && beeps < EDGES)
        off_at[beeps++] = now;
    tone = false;
}

static void run_ms(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now++) {
        if (task && task->tick && (int32_t)(now - task->next_due_ms) >= 0)
            task->tick(task, now);
    }
}

/* The first beep-out's digits as a number: the beeps after the first long
 * one, a digit ending at each gap longer than a beep's own. */
static long heard(void) {
    int i = 0;
    while (i < beeps && off_at[i] - on_at[i] < 400u)
        i++;
    long value = 0;
    int count = 0;
    for (i++; i < beeps && off_at[i] - on_at[i] < 400u; i++) {
        count++;
        bool digit_ends = i + 1 >= beeps || on_at[i + 1] - off_at[i] > 300u || off_at[i + 1] - on_at[i + 1] >= 400u;
        if (digit_ends) {
            value = value * 10 + (count == 10 ? 0 : count);
            count = 0;
        }
    }
    return value;
}

static long beep_out(int32_t v) {
    run_ms(10);
    beeps = 0;
    buzzer_play_altitude(v);
    run_ms(60000u);
    buzzer_stop();
    return heard();
}

void setUp(void) {
    now = 0;
    tone = false;
    beeps = 0;
    buzzer_init();
}

void tearDown(void) {}

void test_beep_out_reads_back_its_digits(void) {
    TEST_ASSERT_EQUAL(1203, beep_out(1203));
}

/* Seven digits do not fit: the most it can say, never the low six. */
void test_beep_out_past_six_digits_says_the_most_it_can(void) {
    TEST_ASSERT_EQUAL_MESSAGE(999999, beep_out(1234567), "the most significant digit was dropped");
}

/* -INT32_MIN does not exist in int32_t. */
void test_beep_out_of_int32_min_is_defined(void) {
    TEST_ASSERT_EQUAL(999999, beep_out(INT32_MIN));
    TEST_ASSERT_EQUAL(250, beep_out(-250));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_beep_out_reads_back_its_digits);
    RUN_TEST(test_beep_out_past_six_digits_says_the_most_it_can);
    RUN_TEST(test_beep_out_of_int32_min_is_defined);
    return UNITY_END();
}
