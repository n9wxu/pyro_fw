/*
 * Ground test command handler — serial commands via the 3.5mm TRRS jack.
 * Implements GND-TEST-01..04, DD-011.
 * SPDX-License-Identifier: MIT
 */
#include "ground_test.h"
#include "flight_states.h" /* flight_context_t, PAD_IDLE, send_telemetry */
#include "buzzer.h"        /* buzzer_set_code, buzzer_set_altitude         */
#include "hal.h"           /* hal_pyro_fire, hal_pyro_check, hal_telemetry_send */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── NMEA-style $GT response helper ──────────────────────────────── */

static void gt_respond(const char *payload) {
    uint8_t chk = 0;
    for (const char *p = payload; *p; p++)
        chk ^= (uint8_t)*p;
    char buf[96];
    snprintf(buf, sizeof(buf), "$%s*%02X\r\n", payload, chk);
    hal_telemetry_send(buf);
}

/* ── Public API ───────────────────────────────────────────────────── */

void ground_test_init(ground_test_ctx_t *gt) {
    gt->arm_state = GT_IDLE;
    gt->arm_time_ms = 0;
}

/* [GND-TEST-03] Auto-disarm: emit timeout notice and return to IDLE */
void ground_test_update(ground_test_ctx_t *gt, uint32_t now_ms) {
    if (gt->arm_state == GT_IDLE)
        return;
    if (gt->arm_time_ms > 0 && (now_ms - gt->arm_time_ms) >= GT_ARM_TIMEOUT_MS) {
        gt->arm_state = GT_IDLE;
        gt->arm_time_ms = 0;
        gt_respond("GT,DISARMED,timeout");
    }
}

/* [GND-TEST-01..04] Parse and execute one command line */
void ground_test_handle_command(ground_test_ctx_t *gt, const char *cmd, struct flight_context_t *ctx, uint32_t now_ms) {
    /* [GND-TEST-04] Commands are only valid in PAD_IDLE */
    if (ctx->current_state != PAD_IDLE) {
        gt_respond("GT,ERR,not_pad_idle");
        return;
    }

    /* ── BEEP STATUS: replay last continuity beep code [GND-TEST-01] ── */
    if (strcmp(cmd, "BEEP") == 0 || strcmp(cmd, "BEEP STATUS") == 0) {
        if (ctx->last_status_code != 0) {
            buzzer_set_code(ctx->last_status_code, false);
            gt_respond("GT,BEEP,status");
        } else {
            gt_respond("GT,ERR,no_status_yet");
        }
        return;
    }

    /* ── BEEP ALT <n>: play altitude beep-out [GND-TEST-01] ── */
    if (strncmp(cmd, "BEEP ALT ", 9) == 0) {
        int32_t alt = (int32_t)atoi(cmd + 9);
        buzzer_set_altitude(alt);
        char resp[32];
        snprintf(resp, sizeof(resp), "GT,BEEP,alt,%ld", (long)alt);
        gt_respond(resp);
        return;
    }

    /* ── ARM <1|2>: arm a channel for test fire [GND-TEST-02..03] ── */
    if (strcmp(cmd, "ARM 1") == 0) {
        if (!ctx->pyro1_continuity_good) {
            gt_respond("GT,ERR,p1_no_cont");
            return;
        }
        gt->arm_state = GT_ARMED_1;
        gt->arm_time_ms = now_ms;
        gt_respond("GT,ARMED,1");
        return;
    }
    if (strcmp(cmd, "ARM 2") == 0) {
        if (!ctx->pyro2_continuity_good) {
            gt_respond("GT,ERR,p2_no_cont");
            return;
        }
        gt->arm_state = GT_ARMED_2;
        gt->arm_time_ms = now_ms;
        gt_respond("GT,ARMED,2");
        return;
    }

    /* ── FIRE <1|2>: fire an armed channel [GND-TEST-02..03] ── */
    if (strcmp(cmd, "FIRE 1") == 0) {
        if (gt->arm_state != GT_ARMED_1) {
            gt_respond("GT,ERR,not_armed");
            return;
        }
        if (gt->arm_time_ms > 0 && (now_ms - gt->arm_time_ms) >= GT_ARM_TIMEOUT_MS) {
            gt->arm_state = GT_IDLE;
            gt_respond("GT,ERR,arm_expired");
            return;
        }
        hal_pyro_fire(1);
        gt->arm_state = GT_IDLE;
        gt->arm_time_ms = 0;
        gt_respond("GT,FIRED,1");
        return;
    }
    if (strcmp(cmd, "FIRE 2") == 0) {
        if (gt->arm_state != GT_ARMED_2) {
            gt_respond("GT,ERR,not_armed");
            return;
        }
        if (gt->arm_time_ms > 0 && (now_ms - gt->arm_time_ms) >= GT_ARM_TIMEOUT_MS) {
            gt->arm_state = GT_IDLE;
            gt_respond("GT,ERR,arm_expired");
            return;
        }
        hal_pyro_fire(2);
        gt->arm_state = GT_IDLE;
        gt->arm_time_ms = 0;
        gt_respond("GT,FIRED,2");
        return;
    }

    /* ── STATUS / ?: send immediate telemetry sentence [GND-TEST-01] ── */
    if (strcmp(cmd, "STATUS") == 0 || strcmp(cmd, "?") == 0) {
        send_telemetry(ctx, 0, ctx->last_altitude, ctx->current_state);
        return;
    }

    /* Unknown command */
    gt_respond("GT,ERR,unknown_cmd");
}
