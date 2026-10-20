/*
 * /api/fs as the board writes it: the flash store and each of its files.
 *
 * Verifies [WEB-API-19].
 */
#include "unity.h"
#include "fs_json.h"
#include "http_conn.h"
#include "vfs_flash.h"
#include "json_check.h"
#include <stdio.h>
#include <string.h>

static char buf[HTTP_WORK_SIZE];

void setUp(void) {}
void tearDown(void) {}

static const fs_space_t SPACE = {4063232u, 120320u, 3500000u, 442912u, 1u, "fast"};

void test_WEB_API_19_the_store_and_each_file(void) {
    fs_json_t j;
    fs_json_begin(&j, buf, (int)sizeof(buf), &SPACE);
    fs_json_file(&j, "config.ini", 812u, true, false);
    fs_json_file(&j, "serial.txt", 13u, true, true);
    fs_json_file(&j, "flight_log.bin", 0u, false, false);
    int n = fs_json_end(&j);
    const char *want = "{\"store\":\"flash\",\"bytes\":4063232,\"in_files\":120320,\"erased\":3500000,"
                       "\"unused\":442912,\"bad_sectors\":1,\"cleanup\":\"fast\",\"files\":["
                       "{\"path\":\"/config.ini\",\"bytes\":812,\"closed\":true,\"board\":false},"
                       "{\"path\":\"/serial.txt\",\"bytes\":13,\"closed\":true,\"board\":true},"
                       "{\"path\":\"/flight_log.bin\",\"bytes\":null,\"closed\":false,\"board\":false}]}";
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    assert_json(buf);
}

void test_WEB_API_19_a_store_with_no_file(void) {
    fs_space_t s = {65536u, 0u, 65536u, 0u, 0u, "done"};
    fs_json_t j;
    fs_json_begin(&j, buf, (int)sizeof(buf), &s);
    TEST_ASSERT_TRUE(fs_json_end(&j) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cleanup\":\"done\",\"files\":[]}"));
    assert_json(buf);
}

/* The most files the store holds, each with the longest name and size,
 * fits the connection's work buffer. */
void test_WEB_API_19_the_widest_report_fits_the_work_buffer(void) {
    fs_space_t s = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, "slow"};
    fs_json_t j;
    fs_json_begin(&j, buf, (int)sizeof(buf), &s);
    char name[ALFS_NAME_MAX + 1];
    for (int i = 0; i < VFS_FLASH_FILES_MAX; i++) {
        memset(name, 'a' + i % 26, ALFS_NAME_MAX);
        name[ALFS_NAME_MAX] = '\0';
        fs_json_file(&j, name, 0xFFFFFFFEu, true, true);
    }
    int n = fs_json_end(&j);
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(buf));
    assert_json(buf);
}

/* A name outside WEB-API-14's characters cannot be asked for by path; it
 * is shown, with each such character a '?', and the JSON stays whole. */
void test_WEB_API_19_a_name_outside_the_path_rules_shows_question_marks(void) {
    fs_json_t j;
    fs_json_begin(&j, buf, (int)sizeof(buf), &SPACE);
    fs_json_file(&j, "a\"b\\c\x01" "d", 1u, true, false);
    TEST_ASSERT_TRUE(fs_json_end(&j) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"path\":\"/a?b?c?d\""));
    assert_json(buf);
}

/* Too small a buffer: refused whole, and nothing written past it. */
void test_WEB_API_19_a_report_that_does_not_fit_is_refused_in_its_buffer(void) {
    memset(buf, '#', sizeof(buf));
    fs_json_t j;
    fs_json_begin(&j, buf, 120, &SPACE);
    fs_json_file(&j, "config.ini", 812u, true, false);
    TEST_ASSERT_EQUAL_INT(-1, fs_json_end(&j));
    for (int i = 120; i < 200; i++)
        TEST_ASSERT_EQUAL_CHAR('#', buf[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_WEB_API_19_the_store_and_each_file);
    RUN_TEST(test_WEB_API_19_a_store_with_no_file);
    RUN_TEST(test_WEB_API_19_the_widest_report_fits_the_work_buffer);
    RUN_TEST(test_WEB_API_19_a_name_outside_the_path_rules_shows_question_marks);
    RUN_TEST(test_WEB_API_19_a_report_that_does_not_fit_is_refused_in_its_buffer);
    return UNITY_END();
}
