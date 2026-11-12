/*
 * The counters in the flight log: all at the open and the close, and between
 * them only what changed, in rows a text record holds.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-15].
 */
#include "unity.h"
#include "../src/log_counters.h"
#include <string.h>

#define ROW 65 /* FLOG_TEXT_MAX and its terminator */

static log_counters_t c;
static uint32_t v[LOG_COUNTER_COUNT];

void setUp(void) {
    memset(&c, 0, sizeof(c));
    memset(v, 0, sizeof(v));
}

void tearDown(void) {}

/* Every row drained into one string, a newline after each. */
static int drain(char *all, int cap) {
    char row[ROW];
    int n, len = 0, rows = 0;
    all[0] = '\0';
    while ((n = log_counters_row(&c, v, row, ROW)) > 0) {
        TEST_ASSERT_TRUE(n < ROW);
        TEST_ASSERT_TRUE(len + n + 2 < cap);
        memcpy(all + len, row, (size_t)n);
        len += n;
        all[len++] = '\n';
        all[len] = '\0';
        rows++;
    }
    return rows;
}

void test_FLT_LOG_15_nothing_changed_is_nothing_said(void) {
    char all[1024];
    log_counters_note(&c, v);
    TEST_ASSERT_EQUAL_INT(0, drain(all, sizeof(all)));
}

void test_FLT_LOG_15_only_what_changed_is_said(void) {
    char all[1024];
    v[LOG_CNT_REFUSALS] = 1;
    v[LOG_CNT_DROPPED] = 22;
    log_counters_note(&c, v);
    TEST_ASSERT_EQUAL_INT(1, drain(all, sizeof(all)));
    TEST_ASSERT_EQUAL_STRING("CNT drop=22 refuse=1\n", all);
    log_counters_note(&c, v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, drain(all, sizeof(all)), "said once, not again while unchanged");
}

/* At the open and the close, every counter, over as many rows as it takes. */
void test_FLT_LOG_15_all_of_them_at_the_open_and_the_close(void) {
    char all[2048];
    for (int i = 0; i < LOG_COUNTER_COUNT; i++) {
        v[i] = 4000000000u - (uint32_t)i;
    }
    log_counters_all(&c);
    int rows = drain(all, sizeof(all));
    TEST_ASSERT_TRUE(rows >= 2);
    TEST_ASSERT_NOT_NULL(strstr(all, " drop=4000000000"));
    TEST_ASSERT_NOT_NULL(strstr(all, " pflash=3999999984"));
    int items = 0;
    for (const char *p = all; (p = strchr(p, '=')) != NULL; p++) {
        items++;
    }
    TEST_ASSERT_EQUAL_INT(LOG_COUNTER_COUNT, items);
}

/* The flash's own activity moves with every page: said at the ends only. */
void test_FLT_LOG_15_the_flash_s_activity_waits_for_the_close(void) {
    char all[1024];
    v[LOG_CNT_PROGRAMS] = 7;
    v[LOG_CNT_WAITS] = 3;
    v[LOG_CNT_PRES_FLASHED] = 2;
    v[LOG_CNT_SUSPENDS] = 1;
    log_counters_note(&c, v);
    TEST_ASSERT_EQUAL_INT(0, drain(all, sizeof(all)));
    log_counters_all(&c);
    drain(all, sizeof(all));
    TEST_ASSERT_NOT_NULL(strstr(all, " prog=7"));
    TEST_ASSERT_NOT_NULL(strstr(all, " pflash=2"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_15_nothing_changed_is_nothing_said);
    RUN_TEST(test_FLT_LOG_15_only_what_changed_is_said);
    RUN_TEST(test_FLT_LOG_15_all_of_them_at_the_open_and_the_close);
    RUN_TEST(test_FLT_LOG_15_the_flash_s_activity_waits_for_the_close);
    return UNITY_END();
}
