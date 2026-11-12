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

#define DIAG_BIT(n) ((uint32_t)1u << (n))
#define DIAG_SENSOR_FAIL DIAG_BIT(0)
#define DIAG_FS_FAIL DIAG_BIT(1)
#define DIAG_STORE_UNPREPARED DIAG_BIT(2)
#define DIAG_P1_OPEN DIAG_BIT(3)
#define DIAG_P1_SHORT DIAG_BIT(4)
#define DIAG_P2_OPEN DIAG_BIT(5)
#define DIAG_P2_SHORT DIAG_BIT(6)
#define DIAG_RESUMED DIAG_BIT(7)
#define DIAG_SENSOR_STUCK DIAG_BIT(8)
#define DIAG_SENSOR_LOST DIAG_BIT(9)
#define DIAG_CONFIG_UNREADABLE DIAG_BIT(10)
#define DIAG_BOARD_MISMATCH DIAG_BIT(11)
#define DIAG_CONFIG_REJECTED DIAG_BIT(12)
#define DIAG_FLIGHT_CODE DIAG_BIT(13)
#define DIAG_FIRES_MOCKED DIAG_BIT(14)
#define DIAG_STORE_CLEANING DIAG_BIT(15)

#define DIAG_PYRO_1 (DIAG_P1_OPEN | DIAG_P1_SHORT)
#define DIAG_PYRO_2 (DIAG_P2_OPEN | DIAG_P2_SHORT)
#define DIAG_PYRO_ANY (DIAG_PYRO_1 | DIAG_PYRO_2)
/* What cannot be corrected at the rocket [BUZ-CODE-02, SNS-PRES-17]. */
#define DIAG_GENERAL                                                                                                   \
    (DIAG_SENSOR_FAIL | DIAG_FS_FAIL | DIAG_SENSOR_STUCK | DIAG_SENSOR_LOST | DIAG_CONFIG_UNREADABLE |                 \
     DIAG_BOARD_MISMATCH | DIAG_CONFIG_REJECTED | DIAG_FLIGHT_CODE | DIAG_FIRES_MOCKED | DIAG_STORE_UNPREPARED)

/* The pyro faults of the enabled channels, as DIAG_* bits. */
uint16_t pad_check_pyro_faults(const hal_continuity_t reading[2], const bool enabled[2]);

/* One announcement, by priority: general, pyro 1, pyro 2, OK. */
beep_reason_t pad_check_announcement(uint16_t diag);

/* [BUZ-07] OK to fly is not said while the store is being prepared: nothing
 * is, and the silence says wait. */
bool pad_check_withholds_ok(uint16_t diag);

const char *pad_check_fault_name(uint16_t diag_bit);

#endif
