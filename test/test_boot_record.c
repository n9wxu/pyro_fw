/*
 * The start-up's record: every fact a post-mortem needs, in rows a log's
 * text record holds.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [FLT-LOG-14].
 */
#include "unity.h"
#include "../src/boot_record.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const boot_facts_t WATCHDOG = {
    "software", true, 7, 4810, "2.3.5", "BY25Q64ES", 8192, false, "resumed in descent", "flight_0012.bin"};

void test_FLT_LOG_14_a_watchdog_reset_names_its_stage(void) {
    boot_rows_t rows;
    boot_record_rows(&WATCHDOG, rows);
    TEST_ASSERT_EQUAL_STRING("BOOT cause=software wd=1 stage=7 at=4810 ms", rows[0]);
    TEST_ASSERT_EQUAL_STRING("BOOT fw=2.3.5 flash=BY25Q64ES 8192kB usb=0", rows[1]);
    TEST_ASSERT_EQUAL_STRING("BOOT start=resumed in descent last=flight_0012.bin", rows[2]);
}

void test_FLT_LOG_14_a_power_reset_with_nothing_recorded(void) {
    boot_facts_t f = WATCHDOG;
    f.cause = "power";
    f.watchdog = false;
    f.stage = -1;
    f.start = "at ground level";
    f.last_log = "";
    boot_rows_t rows;
    boot_record_rows(&f, rows);
    TEST_ASSERT_EQUAL_STRING("BOOT cause=power wd=0 stage=none", rows[0]);
    TEST_ASSERT_EQUAL_STRING("BOOT start=at ground level last=none", rows[2]);
}

/* The widest facts still fit a text row, cut rather than overrun. */
void test_FLT_LOG_14_every_row_fits_a_text_record(void) {
    boot_facts_t f = {"run_pin",
                      true,
                      65535,
                      4294967295u,
                      "2.3.5+local-dirty-0123456789",
                      "XT25F128F-OR-SOME-LONG-PART",
                      4294967295u,
                      true,
                      "altitude unexplained: no reading in time to decide",
                      "flight_9999.bin"};
    boot_rows_t rows;
    boot_record_rows(&f, rows);
    for (int i = 0; i < BOOT_RECORD_ROWS; i++)
        TEST_ASSERT_TRUE(strlen(rows[i]) <= BOOT_RECORD_ROW_MAX);
}

/* The rows as the boot log's lines, or nothing when they do not fit. */
void test_FLT_LOG_14_the_rows_become_the_boot_log_s_lines(void) {
    boot_rows_t rows;
    boot_record_rows(&WATCHDOG, rows);
    char out[256];
    int n = boot_record_lines(rows, out, (int)sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("BOOT cause=software wd=1 stage=7 at=4810 ms\n"
                             "BOOT fw=2.3.5 flash=BY25Q64ES 8192kB usb=0\n"
                             "BOOT start=resumed in descent last=flight_0012.bin\n",
                             out);
    TEST_ASSERT_EQUAL_INT(0, boot_record_lines(rows, out, 60));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_LOG_14_a_watchdog_reset_names_its_stage);
    RUN_TEST(test_FLT_LOG_14_a_power_reset_with_nothing_recorded);
    RUN_TEST(test_FLT_LOG_14_every_row_fits_a_text_record);
    RUN_TEST(test_FLT_LOG_14_the_rows_become_the_boot_log_s_lines);
    return UNITY_END();
}
