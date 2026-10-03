/*
 * Resuming a flight after a restart [FLT-BROWN-01..07, DD-086].
 *
 * A record of the pad, kept while the board sits on it, and the barometer
 * after the restart, answer whether a flight is in progress. The decision is
 * a pure function.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_RESUME_H
#define FLIGHT_RESUME_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RESET_POWER_EVENT = 0, /* power-on or brownout; the processor cannot say which */
    RESET_RUN_PIN,
    RESET_SOFTWARE,
    RESET_DEBUG,
} reset_cause_t;

#define PAD_RECORD_PATH "pad.mkr"
#define PAD_RECORD_DWELL_MS 10000u /* [FLT-BROWN-01] */

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t ground_pressure_pa;
    uint32_t noise_mpa; /* the sensor noise measured on the pad, in mPa */
    uint32_t sum;
} pad_record_t;

void pad_record_fill(pad_record_t *r, int32_t ground_pressure_pa, uint32_t noise_mpa);
bool pad_record_valid(const pad_record_t *r);

typedef enum {
    RESUME_NOT_FLYING = 0,
    RESUME_ASCENT,
    RESUME_DESCENT,
    RESUME_STILL, /* reads high but is not moving: not a flight [FLT-BROWN-03] */
} resume_verdict_t;

/* speed_cms is positive up. */
resume_verdict_t resume_assess(bool record_ok, int32_t height_cm, int32_t speed_cms);
const char *resume_verdict_name(resume_verdict_t v);

/* Why a start did not resume [FLT-BROWN-05]. */
typedef enum {
    NOT_RESUMED_UNDECIDED = 0,
    NOT_RESUMED_NO_RECORD,
    NOT_RESUMED_ON_USB,
    NOT_RESUMED_AT_GROUND,
    NOT_RESUMED_NO_SAMPLE,
} not_resumed_t;

const char *not_resumed_name(not_resumed_t why);

#endif
