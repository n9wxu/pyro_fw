/*
 * What the flight software sends and stores each loop: the pyro backend's
 * step, the telemetry, and the pad record [TEL-03..12, FLT-BROWN-01,
 * FLT-BROWN-04].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_internal.h"
#include "flight_resume.h"
#include "pressure_processing.h"
#include <stdio.h>
#include <string.h>

#define TELEMETRY_MS 1000u    /* [TEL-03] */
#define FAULT_REPORT_MS 5000u /* [TEL-05] */

static uint8_t telemetry_state_id(flight_state_t state) {
    switch (state) {
    case ASCENT:
        return 1;
    case FALLING:
        return 2;
    case DROGUE_DESCENT:
        return 3;
    case CHUTE_DESCENT:
        return 4;
    case LANDED:
        return 5;
    default:
        return 0;
    }
}

static bool state_sends_telemetry(flight_state_t state) {
    return state == PAD_IDLE || state == LANDED || flight_state_is_airborne(state);
}

static void report_fault(flight_context_t *ctx, uint32_t now) {
    if (ctx->last_telemetry != 0 && now - ctx->last_telemetry < FAULT_REPORT_MS)
        return;
    char line[96];
    int n = snprintf(line, sizeof(line), "!FAULT");
    for (uint16_t bit = 1; bit != 0 && n < (int)sizeof(line) - 24; bit <<= 1) {
        if ((ctx->diag & bit) && *pad_check_fault_name(bit))
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", pad_check_fault_name(bit));
    }
    snprintf(line + n, sizeof(line) - (size_t)n, "\r\n");
    hal_telemetry_send(line);
    ctx->last_telemetry = now;
}

static uint8_t telemetry_flags(const flight_context_t *ctx) {
    uint8_t flags = 0;
    if (ctx->channel_ready[0])
        flags |= TELEM_FLAG_P1_READY;
    if (ctx->channel_ready[1])
        flags |= TELEM_FLAG_P2_READY;
    if (ctx->fire.channel[0].fired)
        flags |= TELEM_FLAG_P1_FIRED;
    if (ctx->fire.channel[1].fired)
        flags |= TELEM_FLAG_P2_FIRED;
    if (ctx->pyros_armed)
        flags |= TELEM_FLAG_ARMED;
    if (ctx->apogee_declared)
        flags |= TELEM_FLAG_APOGEE;
    return flags;
}

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    if (ctx->current_state == FAULT) {
        report_fault(ctx, now);
        return;
    }
    if (!state_sends_telemetry(ctx->current_state) || now - ctx->last_telemetry < TELEMETRY_MS)
        return;
    telemetry_snapshot_t snapshot = {
        .seq = ctx->telemetry_seq,
        .state_id = telemetry_state_id(ctx->current_state),
        .thrust = (ctx->current_state == ASCENT && ctx->under_thrust) ? 1u : 0u,
        .flags = telemetry_flags(ctx),
        .alt_cm = ctx->altitude_cm,
        .max_alt_cm = ctx->max_altitude_cm,
        .speed_cms = ctx->speed_cms,
        .press_pa = ctx->pressure_pa,
        .p1_adc = ctx->channel_adc[0],
        .p2_adc = ctx->channel_adc[1],
        .time_ms = flight_elapsed_ms(ctx, now),
    };
    telemetry_send(&ctx->telemetry, &snapshot);
    ctx->telemetry_seq++;
    ctx->last_telemetry = now;
}

static void write_record(const pad_record_t *record) {
    (void)hal_fs_write_file(PAD_RECORD_PATH, (const char *)record, (int)sizeof(*record));
}

/* An invalid record, since the HAL has no delete. */
static void clear_record(void) {
    pad_record_t cleared;
    memset(&cleared, 0, sizeof(cleared));
    write_record(&cleared);
}

/* [FLT-BROWN-04] Cleared once the log has let go of the storage. */
static void clear_record_after_flight(flight_context_t *ctx) {
    if (ctx->record_cleared || hal_log_active())
        return;
    ctx->record_cleared = true;
    clear_record();
}

/* [FLT-BROWN-08] */
static void clear_record_on_the_bench(void) {
    pad_record_t stored;
    int n = hal_fs_read_cached(PAD_RECORD_PATH, (char *)&stored, (int)sizeof(stored));
    if (n == (int)sizeof(stored) && pad_record_valid(&stored))
        clear_record();
}

/* [FLT-BROWN-01] Kept well before the launch it protects against: nothing
 * is stored at launch, when a connector bouncing is likeliest. */
static void keep_record_on_pad(flight_context_t *ctx, uint32_t now) {
    if (ctx->record_written || now - ctx->boot_timer < PAD_RECORD_DWELL_MS)
        return;
    ctx->record_written = true;
    pad_record_t record;
    pad_record_fill(&record, pp_ground_pressure(), (uint32_t)(pp_noise_pa() * 1000.0f));
    write_record(&record);
}

void flight_storage_service(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state == LANDED)
        clear_record_after_flight(ctx);
    else if (ctx->current_state == PAD_IDLE && flight_grounded_on_usb(ctx))
        clear_record_on_the_bench();
    else if (ctx->current_state == PAD_IDLE)
        keep_record_on_pad(ctx, now);
}
