/*
 * The flight logs' names: one file each, one past the highest, the old log
 * kept as the oldest.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-12].
 */
#include "unity.h"
#include "../src/log_files.h"
#include <string.h>

#define HELD_MAX 32
static const char *held[HELD_MAX];
static int held_n, asked;

static bool exists(void *ctx, const char *name) {
    (void)ctx;
    asked++;
    for (int i = 0; i < held_n; i++) {
        if (strcmp(held[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static log_files_t scanned(void) {
    log_files_t f;
    asked = 0;
    log_files_scan(&f, exists, NULL);
    return f;
}

void setUp(void) {
    held_n = 0;
}

void tearDown(void) {}

void test_FLT_LOG_12_an_empty_store_starts_at_one(void) {
    log_files_t f = scanned();
    char name[LOG_FILES_NAME_MAX];
    TEST_ASSERT_FALSE(log_files_newest(&f, name));
    TEST_ASSERT_TRUE(log_files_next(&f, name));
    TEST_ASSERT_EQUAL_STRING("flight_0001.bin", name);
    TEST_ASSERT_TRUE(log_files_newest(&f, name));
    TEST_ASSERT_EQUAL_STRING("flight_0001.bin", name);
}

void test_FLT_LOG_12_a_new_log_goes_past_the_highest_and_past_a_gap(void) {
    held[held_n++] = "flight_0001.bin";
    held[held_n++] = "flight_0002.bin";
    held[held_n++] = "flight_0007.bin"; /* 3 to 6 deleted on their own */
    log_files_t f = scanned();
    char name[LOG_FILES_NAME_MAX];
    TEST_ASSERT_TRUE(log_files_newest(&f, name));
    TEST_ASSERT_EQUAL_STRING("flight_0007.bin", name);
    log_files_next(&f, name);
    TEST_ASSERT_EQUAL_STRING("flight_0008.bin", name);
    log_files_next(&f, name);
    TEST_ASSERT_EQUAL_STRING("flight_0009.bin", name);
}

/* The one slot of before is kept, as the oldest. */
void test_FLT_LOG_12_the_old_single_log_is_the_oldest(void) {
    held[held_n++] = "flight_log.bin";
    log_files_t f = scanned();
    char name[LOG_FILES_NAME_MAX];
    TEST_ASSERT_TRUE(log_files_newest(&f, name));
    TEST_ASSERT_EQUAL_STRING("flight_log.bin", name);
    log_files_next(&f, name);
    TEST_ASSERT_EQUAL_STRING("flight_0001.bin", name);
}

void test_FLT_LOG_12_only_logs_are_logs(void) {
    TEST_ASSERT_TRUE(log_files_is_log("flight_0001.bin"));
    TEST_ASSERT_TRUE(log_files_is_log("flight_9999.bin"));
    TEST_ASSERT_TRUE(log_files_is_log("flight_log.bin"));
    TEST_ASSERT_FALSE(log_files_is_log("flight_0000.bin"));
    TEST_ASSERT_FALSE(log_files_is_log("flight_001.bin"));
    TEST_ASSERT_FALSE(log_files_is_log("flight_00a1.bin"));
    TEST_ASSERT_FALSE(log_files_is_log("flight_0001.bin.part"));
    TEST_ASSERT_FALSE(log_files_is_log("config.ini"));
    TEST_ASSERT_EQUAL_UINT32(42, log_files_number("flight_0042.bin"));
}

/* The scan asks the store a bounded number of times. */
void test_FLT_LOG_12_the_scan_ends_a_gap_past_the_newest(void) {
    held[held_n++] = "flight_0003.bin";
    scanned();
    TEST_ASSERT_EQUAL_INT(1 + 3 + LOG_FILES_GAP, asked);
}

void test_FLT_LOG_12_no_name_once_every_number_is_taken(void) {
    log_files_t f = {LOG_FILES_MAX, false};
    char name[LOG_FILES_NAME_MAX];
    TEST_ASSERT_FALSE(log_files_next(&f, name));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_12_an_empty_store_starts_at_one);
    RUN_TEST(test_FLT_LOG_12_a_new_log_goes_past_the_highest_and_past_a_gap);
    RUN_TEST(test_FLT_LOG_12_the_old_single_log_is_the_oldest);
    RUN_TEST(test_FLT_LOG_12_only_logs_are_logs);
    RUN_TEST(test_FLT_LOG_12_the_scan_ends_a_gap_past_the_newest);
    RUN_TEST(test_FLT_LOG_12_no_name_once_every_number_is_taken);
    return UNITY_END();
}
