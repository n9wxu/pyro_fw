/*
 * No hardware calls: the caller reads the reset registers and the marker.
 *
 * SPDX-License-Identifier: MIT
 */
#include "brownout.h"

/* Not a CRC: the failure to catch is a half-written or erased sector reading
 * back as plausible values, and a sum catches that. */
static uint32_t marker_sum(const pad_marker_t *m) {
    return m->magic ^ (m->version * 2654435761u) ^ (uint32_t)m->ground_pressure_pa ^ (m->sigma_mpa * 40503u);
}

void pad_marker_fill(pad_marker_t *m, int32_t ground_pressure_pa, uint32_t sigma_mpa) {
    m->magic = PAD_MARKER_MAGIC;
    m->version = PAD_MARKER_VERSION;
    m->ground_pressure_pa = ground_pressure_pa;
    m->sigma_mpa = sigma_mpa;
    m->sum = marker_sum(m);
}

bool pad_marker_valid(const pad_marker_t *m) {
    if (!m || m->magic != PAD_MARKER_MAGIC || m->version != PAD_MARKER_VERSION) {
        return false;
    }
    /* A ground pressure outside the range the sensor can see is a marker that
     * did not survive, whatever its sum says. */
    if (m->ground_pressure_pa < 50000 || m->ground_pressure_pa > 110000) {
        return false;
    }
    if (m->sigma_mpa == 0 || m->sigma_mpa > 100000) {
        return false;
    }
    return m->sum == marker_sum(m);
}

recovery_t brownout_assess(reset_cause_t cause, bool marker_ok, int32_t altitude_cm, int32_t speed_cms) {
    /* A software reset is a reboot or an OTA, on the pad, at a keyboard. */
    if (cause != RESET_POWER_EVENT || !marker_ok) {
        return RECOVER_COLD;
    }
    if (altitude_cm <= RECOVER_ALT_CM) {
        return RECOVER_COLD; /* where it was: calibrate, weather and all */
    }
    if (speed_cms >= RECOVER_SPEED_CMS) {
        return RECOVER_ASCENT;
    }
    if (speed_cms <= -RECOVER_SPEED_CMS) {
        return RECOVER_DESCENT;
    }
    /* [FLT-BROWN-03] High but still: a stationary board needs no parachute. */
    return RECOVER_AMBIGUOUS;
}

const char *brownout_cold_name(cold_reason_t w) {
    switch (w) {
    case COLD_NOT_POWER:
        return "cold: not a power event";
    case COLD_NO_MARKER:
        return "cold: no marker";
    case COLD_ON_USB:
        return "cold: on USB";
    case COLD_AT_GROUND:
        return "cold: at ground level";
    case COLD_NO_SAMPLE:
        return "cold: no sample in time";
    case COLD_UNDECIDED:
    default:
        return "cold boot";
    }
}

const char *brownout_recovery_name(recovery_t r) {
    switch (r) {
    case RECOVER_ASCENT:
        return "recovered in ascent";
    case RECOVER_DESCENT:
        return "recovered in descent";
    case RECOVER_AMBIGUOUS:
        return "power event, altitude unexplained";
    case RECOVER_COLD:
    default:
        return "cold boot";
    }
}
