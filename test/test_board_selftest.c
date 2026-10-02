/*
 * The board self-test's decision table.
 *
 * board_selftest_verdict() is the whole decision, and it is pure, so every
 * case it can reach is checkable here without a filesystem or a board. The
 * cases that matter are the two asymmetric ones: a mismatch must FAIL,
 * because committing it leaves the wrong firmware driving the wrong pins;
 * and anything unknown must NOT fail, because refusing to commit on no
 * evidence rolls back good updates.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "board_selftest.h"

/* board_selftest.c reaches the filesystem in its init/save paths. The
 * decision under test does not, so these satisfy the linker and are not
 * called -- a test that needed them would be testing the HAL, not the
 * decision table. */
int hal_fs_read_file(const char *path, char *buf, int max_len) {
    (void)path; (void)buf; (void)max_len;
    return -1;
}
int hal_fs_write_file(const char *path, const char *data, int len) {
    (void)path; (void)data; (void)len;
    return -1;
}

void setUp(void) {}
void tearDown(void) {}

static void test_BST_01_matching_stamp_passes(void) {
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_PASS, board_selftest_verdict("mk1c", "mk1c"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_PASS, board_selftest_verdict("mk1a", "mk1a"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_PASS, board_selftest_verdict("mk1b", "mk1b"));
}

/* The case the whole feature exists for. */
static void test_BST_02_other_board_fails(void) {
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1c", "mk1a"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1a", "mk1c"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1a", "mk1b"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1b", "mk1a"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1b", "mk1c"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1c", "mk1b"));
}

/* No stamp is not a mismatch. A board that has never run firmware this new
 * has nothing to compare, and failing it would refuse the upgrade onto the
 * very version that starts stamping. */
static void test_BST_03_no_stamp_is_unknown_not_a_failure(void) {
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_verdict(NULL, "mk1c"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_verdict("", "mk1c"));
}

/* A build with no token of its own is a build problem, not a board one.
 * Returning FAIL here would roll back every image on every board. */
static void test_BST_04_missing_compiled_token_is_unknown(void) {
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_verdict("mk1c", NULL));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_verdict("mk1c", ""));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_verdict(NULL, NULL));
}

/* Exact comparison. These are machine tokens, so a prefix is a different
 * board and must not pass -- "mk1" is not "mk1a", and a future "mk1c_sd"
 * is not "mk1c". */
static void test_BST_05_comparison_is_exact(void) {
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1", "mk1a"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1a", "mk1"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1c_sd", "mk1c"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("mk1c", "mk1c_sd"));
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_verdict("MK1C", "mk1c"));
}

/* Every pair of the three shipped boards, so adding a board cannot quietly
 * leave a pair unchecked. */
static void test_BST_06_every_shipped_pair(void) {
    static const char *boards[] = {"mk1a", "mk1b", "mk1c"};
    unsigned i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            board_selftest_t want = (i == j) ? BOARD_SELFTEST_PASS : BOARD_SELFTEST_FAIL;
            TEST_ASSERT_EQUAL(want, board_selftest_verdict(boards[i], boards[j]));
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_BST_01_matching_stamp_passes);
    RUN_TEST(test_BST_02_other_board_fails);
    RUN_TEST(test_BST_03_no_stamp_is_unknown_not_a_failure);
    RUN_TEST(test_BST_04_missing_compiled_token_is_unknown);
    RUN_TEST(test_BST_05_comparison_is_exact);
    RUN_TEST(test_BST_06_every_shipped_pair);
    return UNITY_END();
}
