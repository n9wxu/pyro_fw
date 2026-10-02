/*
 * Did this image land on the board it was built for?
 *
 * Every Pyro board is an RP2040, so another board's image installs and runs
 * against the wrong pin map: GPIO25 is MK1A's LED and MK1C's BIAS_B, and
 * MK1C's FIRE_A is MK1A's FIRE2. pyro_safe_all_outputs() safes the pin map the
 * image was compiled for, so it cannot help.
 *
 * The evidence is a stamp, board.txt, that each board writes on its first boot
 * of a firmware that has this. littlefs survives an OTA, a UF2 and a picotool
 * flash alike, so the stamp from the image that ran before is there to compare
 * against at every boot. Not a hardware probe: a sensor that does not answer
 * is equally a dead sensor on the right board.
 *
 * On FAIL the image refuses to arm or fire (hal_board_image_ok()) and says so,
 * at every boot. After an OTA it also declines to commit and reboots, and
 * pico_fota_bootloader puts the previous image -- the one that ran on this
 * board -- back. A board with no stamp yet is UNKNOWN, which neither refuses
 * nor rolls back: there is no evidence to act on.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_SELFTEST_H
#define BOARD_SELFTEST_H

#include <stdbool.h>

typedef enum {
    BOARD_SELFTEST_PASS = 0,
    BOARD_SELFTEST_UNKNOWN, /* no stamp: absence of evidence is not a mismatch */
    BOARD_SELFTEST_FAIL,    /* the stamp names another board */
} board_selftest_t;

/* What to do about the verdict, at boot. */
typedef enum {
    BOARD_IMAGE_RUN,       /* fly; commit an update if there is one */
    BOARD_IMAGE_REFUSE,    /* never arm or fire; nothing to roll back to */
    BOARD_IMAGE_ROLL_BACK, /* never arm or fire; do not commit, reboot */
} board_image_action_t;

/* The verdict, from the two tokens alone. stored may be NULL or empty for "no
 * stamp". Exact comparison: these are machine tokens (BOARD_SHORT_STR). */
board_selftest_t board_selftest_verdict(const char *stored, const char *compiled);

/* The action for a verdict, on a boot that is or is not the first after an
 * update. */
board_image_action_t board_selftest_action(board_selftest_t verdict, bool after_update);

/* Read the stamp and settle the verdict. Safe before hal_fs_mount(), the
 * same way board_identity_init() reads /serial.txt. */
void board_selftest_init(void);

board_selftest_t board_selftest_result(void);

/* The token read from the stamp, or "" if there was none. */
const char *board_selftest_stored(void);

/* True when this board has no stamp and one should be written; the storage
 * task writes it, as board_identity does its own. */
bool board_selftest_unsaved(void);
void board_selftest_save(void);

#endif
