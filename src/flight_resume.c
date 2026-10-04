/*
 * See flight_resume.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_resume.h"

#define PAD_RECORD_MAGIC 0x50594d31u /* "PYM1" */
#define PAD_RECORD_VERSION 3u
#define RESUME_HEIGHT_CM 3000 /* above the recorded ground by more than the launch height */
#define RESUME_SPEED_CMS 500  /* nothing in flight is slower; weather on a pad is */
#define GROUND_PA_MIN 50000
#define GROUND_PA_MAX 110000
#define NOISE_MPA_MAX 100000u

/* Not a CRC: the failure to catch is a half-written or erased sector reading
 * back as plausible values. */
static uint32_t record_sum(const pad_record_t *r) {
    return r->magic ^ (r->version * 2654435761u) ^ (uint32_t)r->ground_pressure_pa ^ (r->noise_mpa * 40503u);
}

void pad_record_fill(pad_record_t *r, int32_t ground_pressure_pa, uint32_t noise_mpa) {
    r->magic = PAD_RECORD_MAGIC;
    r->version = PAD_RECORD_VERSION;
    r->ground_pressure_pa = ground_pressure_pa;
    r->noise_mpa = noise_mpa;
    r->sum = record_sum(r);
}

bool pad_record_valid(const pad_record_t *r) {
    if (!r || r->magic != PAD_RECORD_MAGIC || r->version != PAD_RECORD_VERSION)
        return false;
    if (r->ground_pressure_pa < GROUND_PA_MIN || r->ground_pressure_pa > GROUND_PA_MAX)
        return false;
    if (r->noise_mpa == 0 || r->noise_mpa > NOISE_MPA_MAX)
        return false;
    return r->sum == record_sum(r);
}

resume_verdict_t resume_assess(bool record_ok, int32_t height_cm, int32_t speed_cms) {
    if (!record_ok || height_cm <= RESUME_HEIGHT_CM)
        return RESUME_NOT_FLYING;
    if (speed_cms >= RESUME_SPEED_CMS)
        return RESUME_ASCENT;
    if (speed_cms <= -RESUME_SPEED_CMS)
        return RESUME_DESCENT;
    return RESUME_STILL;
}

const char *resume_verdict_name(resume_verdict_t v) {
    switch (v) {
    case RESUME_ASCENT:
        return "resumed in ascent";
    case RESUME_DESCENT:
        return "resumed in descent";
    case RESUME_STILL:
        return "not resumed: altitude unexplained";
    case RESUME_NOT_FLYING:
    default:
        return "not resumed";
    }
}

const char *not_resumed_name(not_resumed_t why) {
    switch (why) {
    case NOT_RESUMED_NO_RECORD:
        return "not resumed: no record";
    case NOT_RESUMED_ON_USB:
        return "not resumed: on USB";
    case NOT_RESUMED_AT_GROUND:
        return "not resumed: at ground level";
    case NOT_RESUMED_NO_SAMPLE:
        return "not resumed: no sample in time";
    case NOT_RESUMED_UNDECIDED:
    default:
        return "not resumed";
    }
}
