/*
 * See pad_check.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pad_check.h"

static uint16_t fault_of(const hal_continuity_t *reading, uint16_t open_bit, uint16_t short_bit) {
    if (reading->good)
        return 0;
    return reading->open ? open_bit : short_bit;
}

uint16_t pad_check_pyro_faults(const hal_continuity_t reading[2], const bool enabled[2]) {
    uint16_t diag = 0;
    if (enabled[0])
        diag |= fault_of(&reading[0], DIAG_P1_OPEN, DIAG_P1_SHORT);
    if (enabled[1])
        diag |= fault_of(&reading[1], DIAG_P2_OPEN, DIAG_P2_SHORT);
    return diag;
}

beep_reason_t pad_check_announcement(uint16_t diag) {
    if (diag & DIAG_GENERAL)
        return BR_GENERAL_FAULT;
    if (diag & DIAG_PYRO_1)
        return BR_CHECK_PYRO_1;
    if (diag & DIAG_PYRO_2)
        return BR_CHECK_PYRO_2;
    return BR_OK_TO_FLY;
}

const char *pad_check_fault_name(uint16_t diag_bit) {
    switch (diag_bit) {
    case DIAG_SENSOR_FAIL:
        return "sensor_fail";
    case DIAG_FS_FAIL:
        return "fs_fail";
    case DIAG_P1_OPEN:
        return "pyro1_open";
    case DIAG_P1_SHORT:
        return "pyro1_short";
    case DIAG_P2_OPEN:
        return "pyro2_open";
    case DIAG_P2_SHORT:
        return "pyro2_short";
    case DIAG_RESUMED:
        return "flight_resumed";
    case DIAG_SENSOR_STUCK:
        return "sensor_stuck";
    case DIAG_SENSOR_LOST:
        return "sensor_lost";
    default:
        return "";
    }
}
