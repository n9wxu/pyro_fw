/*
 * What each beep means, and which code says it.
 *
 * The codes were thirteen #defines in buzzer.h with their meaning carried
 * only by the macro name. Nothing served them, the web UI never showed them,
 * and an operator at the pad had a two-digit number and no way to look it up.
 * Seven of the thirteen were emitted from nowhere at all.
 *
 * This is the vocabulary: one row per REASON, each with a stable key, a
 * sentence an operator can read, and a code that configuration may change.
 * The firmware asks for a reason and gets whatever code is currently assigned
 * to it -- so renaming a beep never touches the site that emits it.
 *
 * Rules, not I/O. beep_store.c owns the file, the same split pin_assign.c and
 * pin_store.c use, so the rules are testable on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BEEP_CODES_H
#define BEEP_CODES_H

#include <stdbool.h>
#include <stdint.h>

/* ── The reasons ──────────────────────────────────────────────────
 *
 * X(enum_name, ini_key, "what it means")
 *
 * Order is report order: the first matching reason is the one the buzzer
 * plays when several are true at once. Adding a reason is one line here.
 *
 * The keys are what beep.ini carries, so they are part of the file format and
 * do not change once shipped. The sentences are served to the web UI, so the
 * meaning of a code lives in one place rather than in a wiki. */
#define BEEP_REASONS(X)                                                                                                \
    X(BR_ALL_GOOD, "all_good", 1, 1, "Self-test passed: sensor, filesystem and both pyro channels are good")           \
    X(BR_SENSOR_FAIL, "sensor_fail", 4, 1, "No pressure sensor answered. The board cannot detect a launch")            \
    X(BR_FS_FAIL, "fs_fail", 4, 2, "The filesystem did not mount. Config and flight logs are unavailable")             \
    X(BR_CFG_RANGE, "cfg_range", 4, 3, "A pyro altitude setting is above what the sensor can measure")                 \
    X(BR_P1_OPEN, "p1_open", 2, 1, "Pyro 1 reads open: no igniter, or a broken lead")                                  \
    X(BR_P1_SHORT, "p1_short", 2, 2, "Pyro 1 reads shorted")                                                           \
    X(BR_P1_FAULT, "p1_fault", 2, 3, "Pyro 1 reported an overcurrent fault while firing")                              \
    X(BR_P1_NO_OPEN, "p1_no_open", 2, 4, "Pyro 1 did not go open after firing: the charge may not have gone")          \
    X(BR_P2_OPEN, "p2_open", 3, 1, "Pyro 2 reads open: no igniter, or a broken lead")                                  \
    X(BR_P2_SHORT, "p2_short", 3, 2, "Pyro 2 reads shorted")                                                           \
    X(BR_P2_FAULT, "p2_fault", 3, 3, "Pyro 2 reported an overcurrent fault while firing")                              \
    X(BR_P2_NO_OPEN, "p2_no_open", 3, 4, "Pyro 2 did not go open after firing: the charge may not have gone")          \
    X(BR_CRITICAL, "critical", 5, 5, "A failure the firmware could not classify")

#define X_ENUM(name, key, d1, d2, desc) name,
typedef enum { BEEP_REASONS(X_ENUM) BEEP_REASON_COUNT } beep_reason_t;
#undef X_ENUM

/* A digit is beeped as that many beeps, so 0 cannot be heard and a long count
 * cannot be counted. Both digits of every code must be in this range. */
#define BEEP_DIGIT_MIN 1
#define BEEP_DIGIT_MAX 9

typedef struct {
    uint8_t code[BEEP_REASON_COUNT]; /* packed two digits, see buzzer.h */
} beep_table_t;

/* Why a table was refused. */
typedef enum {
    BEEP_OK = 0,
    BEEP_ERR_DIGIT_RANGE, /* a digit outside 1..9: unhearable or uncountable */
    BEEP_ERR_DUPLICATE,   /* two reasons share a code: indistinguishable at the pad */
} beep_err_t;

typedef struct {
    beep_err_t err;
    int reason;       /* the reason the problem is about, or -1 */
    const char *what; /* a short phrase for the operator */
} beep_verdict_t;

/* The codes as shipped. */
void beep_codes_defaults(beep_table_t *t);

/* Check a whole table. Returns the first problem, or BEEP_OK.
 *
 * Whole-table: a half-applied beep map would leave an operator hearing a code
 * that means one thing on the board and another in the UI. */
beep_verdict_t beep_codes_validate(const beep_table_t *t);

/* The code currently assigned to a reason. Never fails, and never returns 0.
 *
 * An entry of 0 means the table has not been loaded -- validation requires
 * both digits in 1..9, so 0 cannot be a real assignment -- and falls back to
 * the shipped code. That keeps this correct before beep_store_load() has run,
 * which matters because a board beeps its self-test result early.
 *
 * An out-of-range reason gets the critical code, because a caller with a bad
 * reason still has something to report. */
uint8_t beep_codes_get(const beep_table_t *t, beep_reason_t r);

/* The code this reason SHIPS with, whatever is assigned now. Served to the
 * UI so its restore button can actually restore rather than pretend. */
uint8_t beep_codes_default(beep_reason_t r);

const char *beep_codes_key(beep_reason_t r);
const char *beep_codes_description(beep_reason_t r);

/* Parse beep.ini over an existing table. Unknown keys are ignored, the same
 * forward-compatibility rule config.ini and pins.ini follow. Mutates buf. */
void beep_codes_parse_ini(char *buf, beep_table_t *t);

/* Serialise. Returns bytes written, or -1 when it would not fit. */
int beep_codes_serialize_ini(const beep_table_t *t, char *buf, int max_len);

const char *beep_codes_strerror(beep_err_t e);

#endif
