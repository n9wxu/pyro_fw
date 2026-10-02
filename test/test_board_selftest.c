/*
 * The board self-test's decisions: the verdict from the stamp, what a boot
 * does about it, and when the stamp may be written.
 *
 * The asymmetric cases are the ones that matter: a mismatch must FAIL and
 * must never fly, because the image drives the wrong pins; and anything
 * unknown must NOT fail, because refusing on no evidence rolls back good
 * updates.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "board_selftest.h"
#include "board_pins.h"
#include "hal.h"
#include <string.h>

/* The stamp board_selftest_init() reads: a file's text, or a failure. */
static const char *stamp_text;
static int stamp_rc;

int hal_fs_read_file(const char *path, char *buf, int max_len) {
    (void)path;
    if (stamp_rc < 0)
        return stamp_rc;
    int n = (int)strlen(stamp_text);
    if (n > max_len)
        n = max_len;
    memcpy(buf, stamp_text, (size_t)n);
    return n;
}
int hal_fs_write_file(const char *path, const char *data, int len) {
    (void)path; (void)data; (void)len;
    return -1;
}

static void boot_with_stamp(const char *text, int rc) {
    stamp_text = text;
    stamp_rc = rc;
    board_selftest_init();
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

/* A mismatch never flies, at every boot: after an OTA it also rolls back,
 * and after a UF2 or picotool flash, with nothing to roll back to, it stays
 * on the ground. */
static void test_BST_07_a_mismatch_never_flies(void) {
    TEST_ASSERT_EQUAL(BOARD_IMAGE_ROLL_BACK, board_selftest_action(BOARD_SELFTEST_FAIL, true));
    TEST_ASSERT_EQUAL(BOARD_IMAGE_REFUSE, board_selftest_action(BOARD_SELFTEST_FAIL, false));
    TEST_ASSERT_EQUAL(BOARD_IMAGE_RUN, board_selftest_action(BOARD_SELFTEST_PASS, true));
    TEST_ASSERT_EQUAL(BOARD_IMAGE_RUN, board_selftest_action(BOARD_SELFTEST_PASS, false));
    TEST_ASSERT_EQUAL(BOARD_IMAGE_RUN, board_selftest_action(BOARD_SELFTEST_UNKNOWN, true));
    TEST_ASSERT_EQUAL(BOARD_IMAGE_RUN, board_selftest_action(BOARD_SELFTEST_UNKNOWN, false));
}

/* Another board's image flashed by hand finds this board's stamp. */
static void test_BST_08_a_hand_flashed_wrong_image_is_refused(void) {
    boot_with_stamp(strcmp(BOARD_SHORT_STR, "mk1a") == 0 ? "mk1c\n" : "mk1a\n", 0);
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_FAIL, board_selftest_result());
    TEST_ASSERT_EQUAL(BOARD_IMAGE_REFUSE, board_selftest_action(board_selftest_result(), false));
    TEST_ASSERT_FALSE_MESSAGE(board_selftest_unsaved(), "the evidence was about to be overwritten");
}

/* Only a stamp that does not exist is written. One that could not be read
 * may be the very stamp that disagrees with this image. */
static void test_BST_09_a_stamp_that_could_not_be_read_is_kept(void) {
    boot_with_stamp("", HAL_FS_ERROR);
    TEST_ASSERT_FALSE(board_selftest_unsaved());
    boot_with_stamp("", HAL_FS_LOCKED);
    TEST_ASSERT_FALSE(board_selftest_unsaved());
    boot_with_stamp("", HAL_FS_NOENT);
    TEST_ASSERT_TRUE(board_selftest_unsaved());
    TEST_ASSERT_EQUAL(BOARD_SELFTEST_UNKNOWN, board_selftest_result());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_BST_01_matching_stamp_passes);
    RUN_TEST(test_BST_02_other_board_fails);
    RUN_TEST(test_BST_03_no_stamp_is_unknown_not_a_failure);
    RUN_TEST(test_BST_04_missing_compiled_token_is_unknown);
    RUN_TEST(test_BST_05_comparison_is_exact);
    RUN_TEST(test_BST_06_every_shipped_pair);
    RUN_TEST(test_BST_07_a_mismatch_never_flies);
    RUN_TEST(test_BST_08_a_hand_flashed_wrong_image_is_refused);
    RUN_TEST(test_BST_09_a_stamp_that_could_not_be_read_is_kept);
    return UNITY_END();
}
