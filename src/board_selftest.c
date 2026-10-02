/*
 * Board self-test. See board_selftest.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_selftest.h"
#include "board_pins.h" /* BOARD_SHORT_STR */
#include "hal.h"

#include <string.h>

#define STAMP_PATH "board.txt"
#define TOKEN_MAX 16

static char stored[TOKEN_MAX];
static board_selftest_t verdict = BOARD_SELFTEST_UNKNOWN;
static bool unsaved;

board_selftest_t board_selftest_verdict(const char *s, const char *compiled) {
    /* No compiled-in token is a build problem, not a board one: FAIL here
     * would roll back every image. */
    if (!compiled || !compiled[0])
        return BOARD_SELFTEST_UNKNOWN;
    if (!s || !s[0])
        return BOARD_SELFTEST_UNKNOWN;
    return strcmp(s, compiled) == 0 ? BOARD_SELFTEST_PASS : BOARD_SELFTEST_FAIL;
}

board_image_action_t board_selftest_action(board_selftest_t verdict, bool after_update) {
    if (verdict != BOARD_SELFTEST_FAIL)
        return BOARD_IMAGE_RUN;
    return after_update ? BOARD_IMAGE_ROLL_BACK : BOARD_IMAGE_REFUSE;
}

/* The stamp is one bare token and nothing else, so a stray newline from an
 * editor or a trailing \r does not read as a different board. */
static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

void board_selftest_init(void) {
    char buf[TOKEN_MAX];
    int n;

    stored[0] = '\0';
    unsaved = false;

    n = hal_fs_read_file(STAMP_PATH, buf, (int)sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        trim(buf);
        /* A stamp longer than a token is not a token: absent, not a
         * mismatch, so a corrupt file does not roll back a good image. */
        if (buf[0] && strlen(buf) < TOKEN_MAX)
            memcpy(stored, buf, strlen(buf) + 1);
    }

    verdict = board_selftest_verdict(stored, BOARD_SHORT_STR);
    /* Stamp a board that has none. Never over a stamp that disagrees -- it
     * is the evidence -- nor over one that could not be read, which may be
     * the same stamp. */
    unsaved = stored[0] == '\0' && (n >= 0 || n == HAL_FS_NOENT);
}

board_selftest_t board_selftest_result(void) {
    return verdict;
}

const char *board_selftest_stored(void) {
    return stored;
}

bool board_selftest_unsaved(void) {
    return unsaved;
}

void board_selftest_save(void) {
    const char *tok = BOARD_SHORT_STR;

    if (!unsaved)
        return;
    if (hal_fs_write_file(STAMP_PATH, tok, (int)strlen(tok)) == 0) {
        memcpy(stored, tok, strlen(tok) + 1);
        verdict = BOARD_SELFTEST_PASS;
        unsaved = false;
    }
}
