/*
 * Surviving a power event in flight [FLT-BROWN-01..06]: the pad marker, and
 * the verdict on a boot, as a pure function. See DD-026.
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

/* ── The pad marker [FLT-BROWN-01, DD-033] ───────────────────────── */
#define PAD_MARKER_MAGIC 0x50594d31u /* "PYM1" */
#define PAD_MARKER_PATH "pad.mkr"
#define PAD_MARKER_VERSION 2u /* 2: the fit's sigma [DD-048] */
#define PAD_MARKER_DWELL_MS 10000u

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t ground_pressure_pa;
    uint32_t sigma_mpa; /* the pad's measured noise, pp_sigma_pa(), in mPa */
    uint32_t sum;       /* over the fields above */
} pad_marker_t;

void pad_marker_fill(pad_marker_t *m, int32_t ground_pressure_pa, uint32_t sigma_mpa);
bool pad_marker_valid(const pad_marker_t *m);

/* ── The verdict ──────────────────────────────────────────────────── */

typedef enum {
    RECOVER_COLD = 0,  /* calibrate as usual; nothing to recover      */
    RECOVER_ASCENT,    /* power event while climbing                  */
    RECOVER_DESCENT,   /* power event while coming down               */
    RECOVER_AMBIGUOUS, /* reads high but is not moving: treated as cold */
} recovery_t;

/* Above the marker's ground by more than this, the board has moved: 30 m is
 * well clear of the barometric noise that set the launch trigger at 100 ft. */
#define RECOVER_ALT_CM 3000

/* [FLT-BROWN-03] Nothing in flight is this slow. Weather can drift a pad by
 * more than 30 m between marker and power-up; motion is what tells it from a
 * flight. */
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
