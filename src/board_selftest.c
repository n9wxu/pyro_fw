/*
 * Board self-test. See board_selftest.h for why it is a stamp and not a
 * hardware probe.
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
    if (!compiled || !compiled[0]) {
        /* No compiled-in token means nothing to compare, which is a build
         * problem rather than a board one. Say UNKNOWN rather than FAIL:
         * refusing to commit here would roll back every image. */
        return BOARD_SELFTEST_UNKNOWN;
    }
    if (!s || !s[0])
        return BOARD_SELFTEST_UNKNOWN;
    return strcmp(s, compiled) == 0 ? BOARD_SELFTEST_PASS : BOARD_SELFTEST_FAIL;
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
        /* A stamp longer than a token is not a token. Treat it as absent
         * rather than as a mismatch: a corrupt file should not roll back a
         * good image. */
        if (buf[0] && strlen(buf) < TOKEN_MAX)
            memcpy(stored, buf, strlen(buf) + 1);
    }

    verdict = board_selftest_verdict(stored, BOARD_SHORT_STR);
    /* Stamp a board that has none, so the next update has something to
     * check. Never overwrite a stamp that disagrees -- that stamp is the
     * evidence, and the image that disagrees with it is the one that should
     * not be here. */
    unsaved = (stored[0] == '\0');
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
