/*
 * Who runs the HTTP work units, and when.
 *
 * Core0 runs one connection's step at a time, in turn, only with a unit's
 * budget left in the period, and always at least one a period. A portable
 * unit core0 had no room for goes to the worker, which takes what fits its
 * grant, holds those connections exclusively, and gives them back only when
 * idle.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-RT-01].
 */
#include "unity.h"
#include "../src/http_work.h"
#include <setjmp.h>
#include <string.h>

static uint32_t clock_us;
uint32_t http_work_clock_us(void) {
    return clock_us;
}

static int ran[HTTP_WORK_SLOTS];
static uint32_t unit_cost;
static jmp_buf killed;
static int kill_in_slot = -1;

static void unit_render(int slot) {
    if (slot == kill_in_slot) {
        longjmp(killed, 1); /* core1 stopped mid-unit */
    }
    ran[slot]++;
    clock_us += unit_cost;
}

const http_unit_fn http_unit_vt[] = {unit_render};

static const bool NONE[HTTP_WORK_SLOTS] = {false, false, false, false};
static const bool ALL[HTTP_WORK_SLOTS] = {true, true, true, true};
#define PLENTY 8000
#define GRANT_FOR(n) ((n) * HTTP_WORKER_UNIT_US + HTTP_WORKER_LUA_MIN_US)

void setUp(void) {
    http_work_reset();
    memset(ran, 0, sizeof(ran));
    unit_cost = 0;
    kill_in_slot = -1;
    clock_us = 1000;
}

void tearDown(void) {
}

static int next(const bool *runnable, int32_t remaining, uint8_t *unit) {
    uint8_t u;
    int s = http_work_next(runnable, remaining, unit ? unit : &u);
    return s;
}

void test_WORK_01_connections_are_served_in_turn(void) {
    http_work_period();
    uint8_t unit;
    for (int i = 0; i < 2 * HTTP_WORK_SLOTS; i++) {
        TEST_ASSERT_EQUAL(i % HTTP_WORK_SLOTS, next(ALL, PLENTY, &unit));
        TEST_ASSERT_EQUAL_HEX8(HTTP_UNIT_NONE, unit);
    }
}

void test_WORK_02_only_connections_with_a_step_are_served(void) {
    http_work_period();
    const bool only2[HTTP_WORK_SLOTS] = {false, false, true, false};
    TEST_ASSERT_EQUAL(-1, next(NONE, PLENTY, NULL));
    TEST_ASSERT_EQUAL(2, next(only2, PLENTY, NULL));
    TEST_ASSERT_EQUAL(2, next(only2, PLENTY, NULL));
}

void test_WORK_03_no_unit_starts_without_its_budget(void) {
    http_work_period();
    TEST_ASSERT_EQUAL(0, next(ALL, PLENTY, NULL));
    TEST_ASSERT_EQUAL(-1, next(ALL, (int32_t)HTTP_UNIT_BUDGET_US - 1, NULL));
    TEST_ASSERT_EQUAL(1, next(ALL, (int32_t)HTTP_UNIT_BUDGET_US, NULL));
}

void test_WORK_04_every_period_runs_at_least_one_unit(void) {
    for (int p = 0; p < 3; p++) {
        http_work_period();
        TEST_ASSERT_EQUAL_MESSAGE(p, next(ALL, 0, NULL), "an overloaded loop must still serve HTTP");
        TEST_ASSERT_EQUAL(-1, next(ALL, 0, NULL));
        TEST_ASSERT_EQUAL(-1, next(ALL, -500, NULL));
    }
}

void test_WORK_05_core0_runs_a_portable_unit_when_it_has_the_budget(void) {
    http_work_period();
    http_work_offer(1, 0);
    TEST_ASSERT_TRUE(http_work_pending(1));
    uint8_t unit;
    TEST_ASSERT_EQUAL(1, next(NONE, PLENTY, &unit));
    TEST_ASSERT_EQUAL_UINT8(0, unit);
    TEST_ASSERT_FALSE(http_work_pending(1));
    TEST_ASSERT_EQUAL(-1, next(NONE, PLENTY, NULL));
}

void test_WORK_06_the_worker_takes_what_core0_had_no_room_for(void) {
    http_work_period();
    TEST_ASSERT_EQUAL(0, next(ALL, PLENTY, NULL));
    http_work_offer(1, 0);
    TEST_ASSERT_EQUAL(-1, next(NONE, (int32_t)HTTP_UNIT_BUDGET_US - 1, NULL));
    TEST_ASSERT_TRUE(http_work_pending(1));

    http_work_period();
    TEST_ASSERT_EQUAL(1, http_work_claim(GRANT_FOR(1)));
    TEST_ASSERT_TRUE(http_work_held(1));
}

/* A connection waiting on its portable unit has nothing else to do. */
void test_WORK_07_a_pending_unit_replaces_the_connections_own_step(void) {
    http_work_period();
    http_work_offer(2, 0);
    const bool only2[HTTP_WORK_SLOTS] = {false, false, true, false};
    uint8_t unit;
    TEST_ASSERT_EQUAL(2, next(only2, PLENTY, &unit));
    TEST_ASSERT_EQUAL_UINT8(0, unit);
}

void test_WORK_08_the_worker_takes_what_fits_its_grant(void) {
    http_work_period();
    http_work_offer(0, 0);
    http_work_offer(1, 0);
    http_work_offer(3, 0);
    TEST_ASSERT_EQUAL(0, http_work_claim(GRANT_FOR(1) - 1));
    TEST_ASSERT_EQUAL(1, http_work_claim(GRANT_FOR(1)));
    TEST_ASSERT_EQUAL(2, http_work_claim(GRANT_FOR(5)));
    TEST_ASSERT_TRUE(http_work_held(0));
    TEST_ASSERT_TRUE(http_work_held(1));
    TEST_ASSERT_FALSE(http_work_held(2));
    TEST_ASSERT_TRUE(http_work_held(3));
    TEST_ASSERT_EQUAL(0, http_work_claim(GRANT_FOR(5)));
}

void test_WORK_09_core0_never_serves_a_held_connection(void) {
    http_work_period();
    http_work_offer(1, 0);
    TEST_ASSERT_EQUAL(1, http_work_claim(GRANT_FOR(1)));
    for (unsigned p = 0; p < 3; p++) {
        http_work_period();
        for (int i = 0; i < 2 * HTTP_WORK_SLOTS; i++) {
            TEST_ASSERT_NOT_EQUAL(1, next(ALL, PLENTY, NULL));
        }
    }
}

void test_WORK_10_the_worker_runs_its_units_and_gives_them_back_when_idle(void) {
    http_work_period();
    http_work_offer(2, 0);
    http_work_claim(GRANT_FOR(1));
    uint8_t lost = 0xFF;
    TEST_ASSERT_EQUAL_HEX8(0, http_work_reclaim(false, &lost));
    TEST_ASSERT_TRUE_MESSAGE(http_work_held(2), "taken back from a busy worker");

    http_work_run();
    TEST_ASSERT_EQUAL(1, ran[2]);
    TEST_ASSERT_EQUAL_HEX8(0, http_work_reclaim(false, &lost));
    TEST_ASSERT_TRUE(http_work_held(2));

    TEST_ASSERT_EQUAL_HEX8(1u << 2, http_work_reclaim(true, &lost));
    TEST_ASSERT_EQUAL_HEX8(0, lost);
    TEST_ASSERT_FALSE(http_work_held(2));
    TEST_ASSERT_FALSE(http_work_pending(2));

    const bool only2[HTTP_WORK_SLOTS] = {false, false, true, false};
    TEST_ASSERT_EQUAL(2, next(only2, PLENTY, NULL));
}

void test_WORK_11_a_unit_runs_once(void) {
    http_work_period();
    http_work_offer(0, 0);
    http_work_claim(GRANT_FOR(1));
    http_work_run();
    http_work_run();
    TEST_ASSERT_EQUAL(1, ran[0]);
}

void test_WORK_12_a_claim_the_worker_never_started_is_pending_again(void) {
    http_work_period();
    http_work_offer(3, 0);
    http_work_claim(GRANT_FOR(1));
    uint8_t lost = 0xFF;
    TEST_ASSERT_EQUAL_HEX8(1u << 3, http_work_reclaim(true, &lost));
    TEST_ASSERT_EQUAL_HEX8(0, lost);
    TEST_ASSERT_FALSE(http_work_held(3));
    TEST_ASSERT_TRUE(http_work_pending(3));
    TEST_ASSERT_EQUAL(1, http_work_claim(GRANT_FOR(1)));
}

/* The worker can die mid-unit: its connection is then half-written. */
void test_WORK_13_a_unit_cut_short_is_reported_lost(void) {
    http_work_period();
    http_work_offer(1, 0);
    http_work_claim(GRANT_FOR(1));
    kill_in_slot = 1;
    if (setjmp(killed) == 0) {
        http_work_run();
        TEST_FAIL_MESSAGE("the unit should not have returned");
    }
    uint8_t lost = 0;
    TEST_ASSERT_EQUAL_HEX8(1u << 1, http_work_reclaim(true, &lost));
    TEST_ASSERT_EQUAL_HEX8(1u << 1, lost);
    TEST_ASSERT_FALSE(http_work_held(1));
    TEST_ASSERT_FALSE(http_work_pending(1));
}

void test_WORK_14_cancel_drops_a_pending_unit_but_not_a_held_one(void) {
    http_work_period();
    http_work_offer(0, 0);
    TEST_ASSERT_TRUE(http_work_cancel(0));
    TEST_ASSERT_FALSE(http_work_pending(0));
    TEST_ASSERT_EQUAL(0, http_work_claim(GRANT_FOR(4)));

    http_work_offer(0, 0);
    http_work_claim(GRANT_FOR(1));
    TEST_ASSERT_FALSE(http_work_cancel(0));
    TEST_ASSERT_TRUE(http_work_held(0));
    TEST_ASSERT_TRUE_MESSAGE(http_work_cancel(1), "nothing pending is nothing to cancel");
}

void test_WORK_15_an_offer_is_made_once(void) {
    http_work_period();
    http_work_offer(2, 0);
    http_work_offer(2, 0);
    TEST_ASSERT_EQUAL(1, http_work_claim(GRANT_FOR(4)));
    http_work_offer(2, 0);
    TEST_ASSERT_FALSE_MESSAGE(http_work_pending(2), "an offer on a held slot is ignored");
}

void test_WORK_16_costs_are_recorded_per_worker(void) {
    http_work_period();
    http_work_offer(0, 0);
    http_work_offer(1, 0);
    http_work_claim(GRANT_FOR(2));
    unit_cost = 700;
    http_work_run();
    http_work_note(HTTP_ON_CORE0, 1200);
    http_work_note(HTTP_ON_CORE0, 300);
    http_work_stats_t st;
    http_work_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(2, st.units[HTTP_ON_WORKER]);
    TEST_ASSERT_EQUAL_UINT32(700, st.max_us[HTTP_ON_WORKER]);
    TEST_ASSERT_EQUAL_UINT32(2, st.units[HTTP_ON_CORE0]);
    TEST_ASSERT_EQUAL_UINT32(1200, st.max_us[HTTP_ON_CORE0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WORK_01_connections_are_served_in_turn);
    RUN_TEST(test_WORK_02_only_connections_with_a_step_are_served);
    RUN_TEST(test_WORK_03_no_unit_starts_without_its_budget);
    RUN_TEST(test_WORK_04_every_period_runs_at_least_one_unit);
    RUN_TEST(test_WORK_05_core0_runs_a_portable_unit_when_it_has_the_budget);
    RUN_TEST(test_WORK_06_the_worker_takes_what_core0_had_no_room_for);
    RUN_TEST(test_WORK_07_a_pending_unit_replaces_the_connections_own_step);
    RUN_TEST(test_WORK_08_the_worker_takes_what_fits_its_grant);
    RUN_TEST(test_WORK_09_core0_never_serves_a_held_connection);
    RUN_TEST(test_WORK_10_the_worker_runs_its_units_and_gives_them_back_when_idle);
    RUN_TEST(test_WORK_11_a_unit_runs_once);
    RUN_TEST(test_WORK_12_a_claim_the_worker_never_started_is_pending_again);
    RUN_TEST(test_WORK_13_a_unit_cut_short_is_reported_lost);
    RUN_TEST(test_WORK_14_cancel_drops_a_pending_unit_but_not_a_held_one);
    RUN_TEST(test_WORK_15_an_offer_is_made_once);
    RUN_TEST(test_WORK_16_costs_are_recorded_per_worker);
    return UNITY_END();
}
