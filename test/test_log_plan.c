/*
 * The three logging plans [FLT-LOG-07, DD-064]: what each keeps, and that
 * the log comes out in time order whatever it keeps.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-07].
 */
#include "unity.h"
#include "../src/flight_events.h"
#include "../src/log_plan.h"
#include <string.h>

#define MAXROWS 20000
static flog_sample_t out[MAXROWS];
static int n_out;

static void emit(void *ctx, const flog_sample_t *s) {
    (void)ctx;
    TEST_ASSERT_TRUE(n_out < MAXROWS);
    out[n_out++] = *s;
}

static log_plan_t plan;

void setUp(void) {
    n_out = 0;
}

void tearDown(void) {
}

/* A flight of `ms` at `period` ms a sample, with an event row at each time in
 * events[] (which must fall on a sample), tagged against that sample. */
static void fly(uint8_t rate, uint32_t ms, uint32_t period, const uint32_t *events, int n_events) {
    log_plan_init(&plan, rate);
    int e = 0;
    for (uint32_t t = 0; t <= ms; t += period) {
        flog_sample_t s = {.time_ms = t, .pressure_pa = (int32_t)t};
        log_plan_take(&plan, &s, emit, NULL);
        while (e < n_events && events[e] == t) {
            s.event = EVT_APOGEE;
            log_plan_take(&plan, &s, emit, NULL);
            e++;
        }
    }
    log_plan_finish(&plan, emit, NULL);
}

static void assert_in_order(void) {
    for (int i = 1; i < n_out; i++) {
        TEST_ASSERT_TRUE_MESSAGE(out[i].time_ms >= out[i - 1].time_ms, "rows out of time order");
    }
}

static int count_events(void) {
    int k = 0;
    for (int i = 0; i < n_out; i++)
        k += out[i].event != EVT_NONE;
    return k;
}

static bool has_sample(uint32_t t) {
    for (int i = 0; i < n_out; i++)
        if (out[i].event == EVT_NONE && out[i].time_ms == t)
            return true;
    return false;
}

void test_PLAN_01_full_keeps_everything(void) {
    const uint32_t ev[] = {5000};
    fly(LOG_RATE_FULL, 10000, 10, ev, 1);
    TEST_ASSERT_EQUAL_INT(1001 + 1, n_out);
    assert_in_order();
}

void test_PLAN_02_1hz_keeps_a_row_a_second_and_every_event(void) {
    const uint32_t ev[] = {2350, 2360, 7770};
    fly(LOG_RATE_1HZ, 10000, 10, ev, 3);
    assert_in_order();
    TEST_ASSERT_EQUAL_INT(3, count_events());
    uint32_t last = 0;
    int rows = 0;
    for (int i = 0; i < n_out; i++) {
        if (out[i].event != EVT_NONE)
            continue;
        if (rows++)
            TEST_ASSERT_EQUAL_UINT32(1000, out[i].time_ms - last);
        last = out[i].time_ms;
    }
    TEST_ASSERT_EQUAL_INT(11, rows);
    for (int i = 0; i < n_out; i++)
        if (out[i].event != EVT_NONE)
            TEST_ASSERT_TRUE_MESSAGE(out[i].time_ms == 2350 || out[i].time_ms == 2360 || out[i].time_ms == 7770,
                                     "an event row at its own time");
}

void test_PLAN_03_events_keeps_every_sample_within_a_second_of_an_event(void) {
    const uint32_t ev[] = {4500};
    fly(LOG_RATE_EVENTS, 10000, 10, ev, 1);
    assert_in_order();
    TEST_ASSERT_EQUAL_INT(1, count_events());
    for (uint32_t t = 3500; t <= 5500; t += 10)
        TEST_ASSERT_TRUE_MESSAGE(has_sample(t), "a sample within a second of the event");
    TEST_ASSERT_FALSE_MESSAGE(has_sample(3490) && has_sample(3480), "full rate starts a second before");
    TEST_ASSERT_FALSE_MESSAGE(has_sample(5510) && has_sample(5520), "and ends a second after");
    /* Away from it, a row a second. */
    int early = 0;
    for (int i = 0; i < n_out; i++)
        early += out[i].time_ms < 3000;
    TEST_ASSERT_TRUE(early >= 3 && early <= 4);
}

void test_PLAN_04_overlapping_windows_and_the_ends(void) {
    const uint32_t ev[] = {200, 1500, 2600, 9900};
    fly(LOG_RATE_EVENTS, 10000, 20, ev, 4);
    assert_in_order();
    TEST_ASSERT_EQUAL_INT(4, count_events());
    for (uint32_t t = 0; t <= 3600; t += 20)
        TEST_ASSERT_TRUE_MESSAGE(has_sample(t), "the windows run together");
    for (uint32_t t = 8900; t <= 10000; t += 20)
        TEST_ASSERT_TRUE_MESSAGE(has_sample(t), "the log's end is flushed");
}

/* Every sample row the plan keeps at the base rate is a second after the
 * last row kept, whatever came between. */
void test_PLAN_05_the_base_rate_resumes_after_a_window(void) {
    const uint32_t ev[] = {3000};
    fly(LOG_RATE_EVENTS, 8000, 10, ev, 1);
    TEST_ASSERT_TRUE(has_sample(4000));
    TEST_ASSERT_TRUE(has_sample(5000));
    TEST_ASSERT_FALSE(has_sample(4010));
    TEST_ASSERT_FALSE(has_sample(4500));
}

/* A stream faster than the line holds: it evicts early, and still keeps
 * order and every event. */
void test_PLAN_06_a_fast_stream_stays_in_order(void) {
    const uint32_t ev[] = {1000, 1001, 1002};
    fly(LOG_RATE_EVENTS, 3000, 1, ev, 3);
    assert_in_order();
    TEST_ASSERT_EQUAL_INT(3, count_events());
    TEST_ASSERT_TRUE(has_sample(1000));
    TEST_ASSERT_TRUE(has_sample(1500));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PLAN_01_full_keeps_everything);
    RUN_TEST(test_PLAN_02_1hz_keeps_a_row_a_second_and_every_event);
    RUN_TEST(test_PLAN_03_events_keeps_every_sample_within_a_second_of_an_event);
    RUN_TEST(test_PLAN_04_overlapping_windows_and_the_ends);
    RUN_TEST(test_PLAN_05_the_base_rate_resumes_after_a_window);
    RUN_TEST(test_PLAN_06_a_fast_stream_stays_in_order);
    return UNITY_END();
}
