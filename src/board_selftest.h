/*
 * Did this image land on the board it was built for?
 *
 * Every Pyro board is an RP2040, so another board's image does not fail to
 * install -- it installs and runs against the wrong pin map. GPIO25 is the
 * heartbeat LED on MK1A and BIAS_B on MK1C, so MK1A firmware on an MK1C
 * board injects bias current into channel B's drain while it believes it is
 * blinking; MK1C's FIRE_A is MK1A's FIRE2. pyro_safe_all_outputs() is no
 * help, because it safes the pin map the firmware was compiled for.
 *
 * The v2.2.1 update page refuses an image whose FILENAME is for another
 * board, which covers the ordinary path. It cannot cover a hand-built
 * image, a renamed one, or a board flashed from something other than that
 * page. This is the device's own answer, and it is the one that holds
 * whatever the image was called.
 *
 * ── Where the evidence comes from ────────────────────────────────
 *
 * The board stamps its own token into the filesystem, which lives in a
 * flash region an OTA does not touch. So the stamp written by the firmware
 * that was running before an update is still there afterwards, and the new
 * image compares it against the board it was compiled for.
 *
 * That is deliberately NOT a hardware probe. The boards are electrically
 * distinguishable -- MK1A's BMP280 is on i2c0, MK1B's on i2c1, MK1C has an
 * MS5607 -- but "the sensor did not answer" is ambiguous: it is equally a
 * dead sensor on the right board, and rolling back a good update because a
 * sensor failed trades one fault for a worse one. A stamp is unambiguous
 * when it is present, and says nothing when it is not.
 *
 * ── What it does on a mismatch ───────────────────────────────────
 *
 * Nothing clever: it declines to commit. pico_fota_bootloader rolls back
 * to the previous image unless the new one commits before its next reboot
 * (hal_firmware_commit()), so withholding the commit and rebooting is the
 * rollback. The previous image is by definition the one that was running on
 * this board, so it is the right one.
 *
 * ── What it cannot do ────────────────────────────────────────────
 *
 * A board with no stamp yet -- never run firmware this new, or a freshly
 * formatted filesystem -- returns UNKNOWN, and an UNKNOWN commits. There is
 * no evidence to refuse on, and refusing everything unstamped would brick
 * the upgrade onto this very version. Each board stamps itself on its first
 * boot of a firmware that has this, so the gap closes after one boot per
 * board, which is why the filename check in the page still matters.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_SELFTEST_H
#define BOARD_SELFTEST_H

#include <stdbool.h>

typedef enum {
    /* The stamp agrees with the board this image was compiled for. */
    BOARD_SELFTEST_PASS = 0,
    /* No stamp to compare against. Commits: absence of evidence is not
     * evidence of a mismatch. */
    BOARD_SELFTEST_UNKNOWN,
    /* The stamp names a different board. This image is on hardware it was
     * not built for. Does not commit. */
    BOARD_SELFTEST_FAIL,
} board_selftest_t;

/* The verdict, from the two tokens alone. Pure, so the whole decision table
 * is testable without a filesystem or a board.
 *
 * stored may be NULL or empty for "no stamp". Comparison is exact: the
 * tokens are machine identifiers (BOARD_SHORT_STR), not display names, so
 * there is nothing to normalise and nothing to guess at. */
board_selftest_t board_selftest_verdict(const char *stored, const char *compiled);

/* Read the stamp and settle the verdict. Safe before hal_fs_mount(), the
 * same way board_identity_init() reads /serial.txt. */
void board_selftest_init(void);

board_selftest_t board_selftest_result(void);

/* The token read from the stamp, or "" if there was none. For reporting:
 * a mismatch is worth saying out loud, with both names. */
const char *board_selftest_stored(void);

/* True when this board has no stamp and one should be written. Deferred the
 * way board_identity defers its own write, so the flash write happens where
 * the system already allows one rather than during bring-up. */
bool board_selftest_unsaved(void);
void board_selftest_save(void);

#endif
