/*
 * Ground test command handler — serial commands via the 3.5mm TRRS jack.
 *
 * Implements GND-TEST-01..04 and DD-011:
 *   BEEP STATUS        — replay the last continuity status beep code
 *   BEEP ALT <n>       — play altitude beep-out for value n (in configured units)
 *   ARM <1|2>          — arm a pyro channel for ground test (3s auto-disarm)
 *   FIRE <1|2>         — fire an armed channel (requires preceding ARM)
 *   STATUS             — send an immediate telemetry sentence
 *
 * All commands are rejected outside PAD_IDLE state [GND-TEST-04].
 * ARM → FIRE must occur within 3 seconds [GND-TEST-03].
 * Responses are $GT,... NMEA-style sentences sent via hal_telemetry_send().
 *
 * ground_test_ctx_t is embedded in flight_context_t so the state survives
 * between dispatch_state() calls.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GROUND_TEST_H
#define GROUND_TEST_H

#include <stdint.h>
#include <stdbool.h>

/* ── Ground test arm state machine ───────────────────────────────── */

typedef enum {
    GT_IDLE = 0, /* no channel armed                    */
    GT_ARMED_1,  /* channel 1 armed, 3s timer running  */
    GT_ARMED_2,  /* channel 2 armed, 3s timer running  */
} ground_test_arm_t;

typedef struct {
    ground_test_arm_t arm_state;
    uint32_t arm_time_ms; /* when ARM was accepted */
} ground_test_ctx_t;

/* Forward declaration avoids circular include with flight_states.h */
struct flight_context_t;

/* Initialise (called from flight_init) */
void ground_test_init(ground_test_ctx_t *gt);

/* Process one complete serial command line (NUL-terminated, no CR/LF).
 * Called from detect_pad_idle() when hal_serial_readline() returns true.
 * ctx must be the owning flight_context_t. [GND-TEST-01..04] */
void ground_test_handle_command(ground_test_ctx_t *gt, const char *cmd, struct flight_context_t *ctx, uint32_t now_ms);

/* Auto-disarm check — call every tick from detect_pad_idle().
 * Emits $GT,DISARMED,timeout if 3s have elapsed without a FIRE. */
void ground_test_update(ground_test_ctx_t *gt, uint32_t now_ms);

/* 3-second arm window [GND-TEST-03] */
#define GT_ARM_TIMEOUT_MS 3000U

#endif
