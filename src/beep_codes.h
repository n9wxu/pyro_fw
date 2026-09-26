/*
 * What the buzzer says, and how it says it.
 *
 * A beep tells an operator what to DO. There are four things they can do
 * standing at a rocket -- fly it, check igniter 1, check igniter 2, or safe it
 * and walk away -- so there are four outcomes. The diagnosis is finer than
 * that and lives on /api/status, which is read on a screen.
 *
 * MATCHING WHAT A FLIER ALREADY KNOWS
 *
 * The shipped personality follows Eggtimer Rocketry, whose convention most
 * fliers at a launch site already have in their ear:
 *
 *   ready to fly   a rapid continuous chirp, never counted
 *                  (Eggtimer Quantum 1.09G, "rapid chirping... high-pitched
 *                   warble"; Quark "frantic chirping")
 *   no continuity  a counted code repeating until fixed
 *                  (Eggtimer Quark: 4 = no Main, 5 = no Drogue)
 *   hardware fault 2 beeps
 *                  (Eggtimer Classic/TRS: 2 = Altimeter Sensor or Hardware)
 *
 * The good case is a TEXTURE, not a number. Asking someone to count the case
 * that means "everything is fine" gets it wrong in the wind -- and a vendor
 * thread shows fliers misreading even a vendor's own counted codes badly
 * enough that an audio recording was needed to diagnose it.
 *
 * Neither NAR nor Tripoli specifies anything about audible status, so there is
 * no standard to comply with, only a convention to match.
 *
 * PERSONALITIES
 *
 * Three slots. Slot 0 ships as the Eggtimer convention; the other two are the
 * operator's to name and shape. One is active. A personality is a complete
 * configuration -- pattern per outcome, cadence, and whether pyro faults are
 * split per channel -- because those choices only make sense together.
 *
 * Rules, not I/O. beep_store.c owns the file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BEEP_CODES_H
#define BEEP_CODES_H

#include <stdbool.h>
#include <stdint.h>

/* ── The outcomes ─────────────────────────────────────────────────
 *
 * X(enum_name, ini_key, "what it means")
 *
 * Order is priority: the first that applies is the one played. Anything
 * unfixable at the pad outranks anything fixable, because adjusting an
 * igniter does not help a board that cannot fly. */
#define BEEP_REASONS(X)                                                                                                \
    X(BR_SYSTEM_FAILURE, "system_failure",                                                                             \
      "System failure. Safe the system and leave the pad -- this cannot be fixed at the rocket")                       \
    X(BR_CHECK_PYRO_1, "check_pyro_1", "Check pyro 1. Its igniter or leads need attention")                            \
    X(BR_CHECK_PYRO_2, "check_pyro_2", "Check pyro 2. Its igniter or leads need attention")                            \
    X(BR_OK_TO_FLY, "ok_to_fly", "OK to fly. Sensor, filesystem and both pyro channels are good")

#define X_ENUM(name, key, desc) name,
typedef enum { BEEP_REASONS(X_ENUM) BEEP_REASON_COUNT } beep_reason_t;
#undef X_ENUM

/* ── How an outcome sounds ────────────────────────────────────────
 *
 * A kind, not just digits. Eggtimer's ready-to-fly cannot be written as a
 * pair of counts, and that is the point: the good case is meant to be
 * recognised, not counted. */
typedef enum {
    BK_SILENT = 0, /* say nothing                                    */
    BK_CHIRP,      /* rapid continuous warble -- Eggtimer "ready"     */
    BK_TONE,       /* an unbroken tone -- Eggtimer Quantum "do not fly" */
    BK_CODE,       /* counted beeps, one or two groups                */
} beep_kind_t;

/* A digit is beeped as that many beeps, so 0 cannot be heard and a long count
 * cannot be counted. d2 == 0 means a single group, which is the Eggtimer
 * shape; a non-zero d2 gives the two-group form. */
#define BEEP_DIGIT_MIN 1
#define BEEP_DIGIT_MAX 9

typedef struct {
    uint8_t kind; /* beep_kind_t */
    uint8_t d1;   /* BK_CODE: first group, 1..9                */
    uint8_t d2;   /* BK_CODE: second group, 0 = no second group */
} beep_spec_t;

#define BEEP_NAME_MAX 13
#define BEEP_PERSONALITY_COUNT 3

typedef struct {
    char name[BEEP_NAME_MAX];
    beep_spec_t spec[BEEP_REASON_COUNT];

    /* Silence must mean something is wrong, not "you missed it". A board that
     * says its state once and stops is indistinguishable from one whose
     * battery died a second later. */
    uint16_t gap_ms;  /* between re-announcements; 0 = no gap, run together */
    uint8_t repeat;   /* 0 = until launch, N = N times then stop            */
    bool split_pyro;  /* false: either channel uses check_pyro_1            */
} beep_personality_t;

typedef struct {
    beep_personality_t p[BEEP_PERSONALITY_COUNT];
    uint8_t active;
} beep_table_t;

/* Why a table was refused. */
typedef enum {
    BEEP_OK = 0,
    BEEP_ERR_DIGIT_RANGE,  /* a digit outside 1..9: unhearable or uncountable */
    BEEP_ERR_DUPLICATE,    /* two outcomes sound the same                     */
    BEEP_ERR_NO_ACTIVE,    /* the active slot is not one that exists          */
    BEEP_ERR_ALL_SILENT,   /* a personality that says nothing at all          */
    /* Not a verdict on the table: it was valid and could not be stored. */
    BEEP_ERR_TOO_LARGE, /* serialised, it does not fit beep.ini's budget   */
    BEEP_ERR_STORE,     /* the write to beep.ini failed                    */
} beep_err_t;

typedef struct {
    beep_err_t err;
    int personality;  /* which slot, or -1 */
    int reason;       /* which outcome, or -1 */
    const char *what; /* a short phrase for the operator */
} beep_verdict_t;

/* The shipped table: slot 0 is the Eggtimer convention, 1 and 2 are copies
 * for the operator to reshape. */
void beep_codes_defaults(beep_table_t *t);

/* Check the whole table. Returns the first problem, or BEEP_OK.
 *
 * Whole-table: a half-applied beep map would leave an operator hearing
 * something that means one thing on the board and another in the UI. */
beep_verdict_t beep_codes_validate(const beep_table_t *t);

/* The active personality. Never NULL. */
const beep_personality_t *beep_codes_active(const beep_table_t *t);

/* How an outcome sounds under the active personality. Honours split_pyro:
 * with it off, check_pyro_2 is never returned. */
beep_spec_t beep_codes_spec(const beep_table_t *t, beep_reason_t r);

const char *beep_codes_key(beep_reason_t r);
const char *beep_codes_description(beep_reason_t r);
const char *beep_codes_kind_name(beep_kind_t k);

/* Parse beep.ini over an existing table. Unknown keys are ignored, the same
 * forward-compatibility rule config.ini and pins.ini follow. Mutates buf. */
void beep_codes_parse_ini(char *buf, beep_table_t *t);

/* Serialise. Returns bytes written, or -1 when it would not fit. */
int beep_codes_serialize_ini(const beep_table_t *t, char *buf, int max_len);

const char *beep_codes_strerror(beep_err_t e);

#endif
