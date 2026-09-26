/*
 * Surviving a power event in flight.
 *
 * A brownout is electrically a power cycle: the RP2040's brown-out detector
 * drives POR, every RAM contents and every watchdog scratch register is gone,
 * and the reset registers cannot tell a brownout from someone plugging the
 * battery in. So the reset cause alone can never answer "did we just fall out
 * of the sky and come back". It narrows the question to "this was a power
 * event"; a marker written to flash while the board sat on the pad, plus what
 * the barometer says now, answers the rest.
 *
 * The decision is a pure function so the whole matrix can be tested on the
 * host, including the cases that would need a rocket to reproduce.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BROWNOUT_H
#define BROWNOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RESET_POWER_EVENT = 0, /* power-on OR brownout; the silicon cannot say */
    RESET_RUN_PIN,
    RESET_SOFTWARE, /* watchdog, or a deliberate reboot from the web UI */
    RESET_DEBUG,
} reset_cause_t;

/* ── The pad marker ───────────────────────────────────────────────
 *
 * Written once, after the board has sat still on the pad for ten seconds, and
 * never during ascent. Launch shock is the most likely cause of the brownout
 * this exists to survive -- a battery connector bouncing -- and a flash write
 * in progress is the worst moment to lose power. */
#define PAD_MARKER_MAGIC 0x50594d31u /* "PYM1" */
#define PAD_MARKER_PATH "pad.mkr"
#define PAD_MARKER_VERSION 1u
#define PAD_MARKER_DWELL_MS 10000u

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t ground_pressure_pa;
    uint32_t sum; /* over the three fields above */
} pad_marker_t;

void pad_marker_fill(pad_marker_t *m, int32_t ground_pressure_pa);
bool pad_marker_valid(const pad_marker_t *m);

/* ── The verdict ──────────────────────────────────────────────────── */

typedef enum {
    RECOVER_COLD = 0,  /* calibrate as usual; nothing to recover      */
    RECOVER_ASCENT,    /* power event while climbing                  */
    RECOVER_DESCENT,   /* power event while coming down               */
    RECOVER_AMBIGUOUS, /* reads high but is not moving: treated as cold */
} recovery_t;

/* Above the recorded ground by more than this, the board is not where it was
 * when the marker was written. 30 m is well clear of the barometric noise
 * that put the launch trigger at 100 ft. */
#define RECOVER_ALT_CM 3000

/* Nothing in flight is this slow. The motion test is what makes the altitude
 * test safe: weather can drift the pressure by more than 30 m of altitude
 * between the marker being written and the board being switched on again, and
 * without this a drifting barometer on the pad would look airborne -- which
 * would arm the pyros of a rocket somebody is standing next to. */
#define RECOVER_SPEED_CMS 500

/* speed_cms is signed: positive is up. */
recovery_t brownout_assess(reset_cause_t cause, bool marker_ok, int32_t altitude_cm, int32_t speed_cms);

/* A phrase for /api/status and the flight log. */
const char *brownout_recovery_name(recovery_t r);

/* Why a boot was cold [FLT-BROWN-05]. "At ground level" is the one that proves
 * the barometer was read: every other reason is decided without a sample. */
typedef enum {
    COLD_UNDECIDED = 0,
    COLD_NOT_POWER, /* a reset, not a power event */
    COLD_NO_MARKER, /* no valid pad marker */
    COLD_ON_USB,    /* a host on the port: a bench, not a flight [USB-01] */
    COLD_AT_GROUND, /* read, and within 30 m of the marker's ground */
    COLD_NO_SAMPLE, /* the deadline came before enough readings */
} cold_reason_t;

const char *brownout_cold_name(cold_reason_t w);

#endif
