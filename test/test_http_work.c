/*
 * The HTTP work units' turn [WEB-HTTP-06]: one connection's step at a time,
 * in turn, only with a unit's budget left in the period, and always at least
 * one a period.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/http_work.h"
#include <string.h>

static int ran[HTTP_WORK_SLOTS];

static void unit_render(int slot) {
    ran[slot]++;
}

const http_unit_fn http_unit_vt[] = {unit_render};

static const bool NONE[HTTP_WORK_SLOTS] = {false, false, false, false};
static const bool ALL[HTTP_WORK_SLOTS] = {true, true, true, true};
#define PLENTY 8000

void setUp(void) {
    http_work_reset();
    memset(ran, 0, sizeof(ran));
}

void tearDown(void) {
}

static int next(const bool *runnable, int32_t remaining, uint8_t *unit) {
    uint8_t u;
    return http_work_next(runnable, remaining, unit ? unit : &u);
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
        TEST_ASSERT_EQUAL_MESSAGE(p, next(ALL, 0, NULL), "an overloaded period must still serve HTTP");
        TEST_ASSERT_EQUAL(-1, next(ALL, 0, NULL));
        TEST_ASSERT_EQUAL(-1, next(ALL, -500, NULL));
    }
}

void test_WORK_05_an_offered_unit_runs_when_the_budget_allows(void) {
    http_work_period();
    http_work_offer(1, 0);
    TEST_ASSERT_TRUE(http_work_pending(1));
    uint8_t unit;
    TEST_ASSERT_EQUAL(1, next(NONE, PLENTY, &unit));
    TEST_ASSERT_EQUAL_UINT8(0, unit);
    TEST_ASSERT_FALSE(http_work_pending(1));
    TEST_ASSERT_EQUAL(-1, next(NONE, PLENTY, NULL));
}

void test_WORK_06_a_unit_with_no_budget_waits_for_the_next_period(void) {
    http_work_period();
    TEST_ASSERT_EQUAL(0, next(ALL, PLENTY, NULL));
    http_work_offer(1, 0);
    TEST_ASSERT_EQUAL(-1, next(NONE, (int32_t)HTTP_UNIT_BUDGET_US - 1, NULL));
    TEST_ASSERT_TRUE(http_work_pending(1));

    http_work_period();
    uint8_t unit;
    TEST_ASSERT_EQUAL(1, next(NONE, 0, &unit));
    TEST_ASSERT_EQUAL_UINT8(0, unit);
}

/* A connection waiting on its unit has nothing else to do. */
void test_WORK_07_a_pending_unit_replaces_the_connections_own_step(void) {
    http_work_period();
    http_work_offer(2, 0);
    const bool only2[HTTP_WORK_SLOTS] = {false, false, true, false};
    uint8_t unit;
    TEST_ASSERT_EQUAL(2, next(only2, PLENTY, &unit));
    TEST_ASSERT_EQUAL_UINT8(0, unit);
}

void test_WORK_08_cancel_drops_a_pending_unit(void) {
    http_work_period();
    http_work_offer(0, 0);
    http_work_cancel(0);
    TEST_ASSERT_FALSE(http_work_pending(0));
    TEST_ASSERT_EQUAL(-1, next(NONE, PLENTY, NULL));
}

void test_WORK_09_an_offer_is_made_once(void) {
    http_work_period();
    http_work_offer(2, 0);
    http_work_offer(2, 0);
    TEST_ASSERT_EQUAL(2, next(NONE, PLENTY, NULL));
    TEST_ASSERT_EQUAL(-1, next(NONE, PLENTY, NULL));
}

void test_WORK_10_costs_are_recorded(void) {
    http_work_note(1200);
    http_work_note(300);
    http_work_stats_t st;
    http_work_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(2, st.units);
    TEST_ASSERT_EQUAL_UINT32(1200, st.max_us);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WORK_01_connections_are_served_in_turn);
    RUN_TEST(test_WORK_02_only_connections_with_a_step_are_served);
    RUN_TEST(test_WORK_03_no_unit_starts_without_its_budget);
    RUN_TEST(test_WORK_04_every_period_runs_at_least_one_unit);
    RUN_TEST(test_WORK_05_an_offered_unit_runs_when_the_budget_allows);
    RUN_TEST(test_WORK_06_a_unit_with_no_budget_waits_for_the_next_period);
    RUN_TEST(test_WORK_07_a_pending_unit_replaces_the_connections_own_step);
    RUN_TEST(test_WORK_08_cancel_drops_a_pending_unit);
    RUN_TEST(test_WORK_09_an_offer_is_made_once);
    RUN_TEST(test_WORK_10_costs_are_recorded);
    return UNITY_END();
}
