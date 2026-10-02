/*
 * Ground test commands on the telemetry UART's RX, the line telemetry goes out
 * on (each board's THEORY_OF_OPERATION.md names its connector)
 * [GND-TEST-01..04, DD-011]:
 *   BEEP STATUS        — replay the last continuity status beep code
 *   BEEP ALT <n>       — play altitude beep-out for value n (in configured units)
 *   ARM <1|2>          — arm a pyro channel for ground test (3s auto-disarm)
 *   FIRE <1|2>         — fire an armed channel (requires preceding ARM)
 *   STATUS             — send an immediate telemetry sentence
 *
 * Every command is refused outside PAD_IDLE [GND-TEST-04], and FIRE more than
 * 3 s after ARM [GND-TEST-03]. Replies are $GT sentences; a line that starts
 * '$' or '!', as everything this board sends does, is not a command.
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

struct flight_context_t; /* flight_states.h includes this header */

void ground_test_init(ground_test_ctx_t *gt);

/* One line from hal_serial_readline(), NUL-terminated, without CR/LF; ctx is
 * the flight context that owns gt. */
void ground_test_handle_command(ground_test_ctx_t *gt, const char *cmd, struct flight_context_t *ctx, uint32_t now_ms);

/* Every tick on the pad: $GT,DISARMED,timeout once 3 s pass without FIRE. */
void ground_test_update(ground_test_ctx_t *gt, uint32_t now_ms);

/* 3-second arm window [GND-TEST-03] */
#define GT_ARM_TIMEOUT_MS 3000U

#endif
