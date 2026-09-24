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

/* ── The three outcomes ───────────────────────────────────────────
 *
 * X(enum_name, ini_key, d1, d2, "what it means")
 *
 * A beep says what to DO, not what is wrong. There are only three things an
 * operator can do at the pad, so there are only three beeps:
 *
 *   OK to fly        proceed
 *   Check the pyro   an igniter can be reached and adjusted without safing
 *   System failure   safe the system and leave the pad
 *
 * Thirteen codes distinguished p1_open from p2_short from fs_fail, which is a
 * distinction that changes nothing an operator does standing at the rocket --
 * and asked them to count two groups of beeps in the wind and then look the
 * pair up. The diagnosis still exists and is still reported: it goes to
 * /api/status, which is read on a screen where detail helps.
 *
 * Order is priority. A board with a dead sensor AND an open igniter must send
 * the operator away, so SYSTEM_FAILURE outranks CHECK_PYRO.
 *
 * The keys are what beep.ini carries, so they are part of the file format.
 * The sentences are served to the web UI, so a code's meaning lives in one
 * place. */
#define BEEP_REASONS(X)                                                                                                \
    X(BR_SYSTEM_FAILURE, "system_failure", 3, 3,                                                                       \
      "System failure. Safe the system and leave the pad -- this cannot be fixed at the rocket")                       \
    X(BR_CHECK_PYRO, "check_pyro", 2, 2,                                                                               \
      "Check the pyro. An igniter or its leads need attention; the rest of the board is good")                         \
    X(BR_OK_TO_FLY, "ok_to_fly", 1, 1, "OK to fly. Sensor, filesystem and both pyro channels are good")

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
