/*
 * What is wrong on the pad, and which of the four announcements says so
 * [BUZ-01, BUZ-CODE-02, PYR-CONT-02, PYR-HEALTH-01, PYR-HEALTH-02, DD-083].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PAD_CHECK_H
#define PAD_CHECK_H

#include "beep_codes.h"
#include "hal.h"
#include <stdbool.h>
#include <stdint.h>

#define DIAG_SENSOR_FAIL (1u << 0)
#define DIAG_FS_FAIL (1u << 1)
#define DIAG_P1_OPEN (1u << 3)
#define DIAG_P1_SHORT (1u << 4)
#define DIAG_P2_OPEN (1u << 5)
#define DIAG_P2_SHORT (1u << 6)
#define DIAG_RESUMED (1u << 7)
#define DIAG_SENSOR_STUCK (1u << 8)
#define DIAG_SENSOR_LOST (1u << 9)

#define DIAG_PYRO_1 (DIAG_P1_OPEN | DIAG_P1_SHORT)
#define DIAG_PYRO_2 (DIAG_P2_OPEN | DIAG_P2_SHORT)
#define DIAG_PYRO_ANY (DIAG_PYRO_1 | DIAG_PYRO_2)
/* What cannot be corrected at the rocket [BUZ-CODE-02, SNS-PRES-17]. */
#define DIAG_GENERAL (DIAG_SENSOR_FAIL | DIAG_FS_FAIL | DIAG_SENSOR_STUCK | DIAG_SENSOR_LOST)

/* The pyro faults of the enabled channels, as DIAG_* bits. */
uint16_t pad_check_pyro_faults(const hal_continuity_t reading[2], const bool enabled[2]);

/* One announcement, by priority: general, pyro 1, pyro 2, OK. */
beep_reason_t pad_check_announcement(uint16_t diag);

const char *pad_check_fault_name(uint16_t diag_bit);

#endif
