/*
 * The serial telemetry (telemetry_formatter.h). Sentences and fields:
 * docs/ground-station-interface-spec.md §5 and §6.
 *
 * SPDX-License-Identifier: MIT
 */
#include "telemetry_formatter.h"
#include "hal.h"
#include "flight_states.h"
#include <stdio.h>

static const config_t *s_cfg = NULL;

void telemetry_init(const config_t *cfg) {
    s_cfg = cfg;
}

/* ── NMEA helpers ─────────────────────────────────────────────────── */

static void nmea_send(const char *payload) {
    uint8_t ck = 0;
    for (int i = 0; payload[i] != '\0'; i++)
        ck ^= (uint8_t)payload[i];
    char sentence[280];
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, ck);
    hal_telemetry_send(sentence);
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static void json_send(const char *obj) {
    char line[280];
    snprintf(line, sizeof(line), "%s\r\n", obj);
    hal_telemetry_send(line);
}

/* ── telemetry_state ──────────────────────────────────────────────── */

void telemetry_state(const telemetry_snapshot_t *s) {
    if (!s_cfg)
        return;

    if (s_cfg->telem_format == TELEM_FORMAT_NMEA) {
        /* The last two, battery and temperature, are not measured. */
        char payload[200];
        snprintf(payload, sizeof(payload), "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%d,%d", (unsigned)s->seq,
                 (unsigned)s->state_id, (unsigned)s->thrust, (long)s->alt_cm, (long)s->speed_cms, (long)s->max_alt_cm,
                 (long)s->press_pa, (unsigned long)s->time_ms, (unsigned)s->flags, (unsigned)s->p1_adc,
                 (unsigned)s->p2_adc, 0, 0);
        nmea_send(payload);

    } else if (s_cfg->telem_format == TELEM_FORMAT_JSON) {
        char obj[280];
        snprintf(obj, sizeof(obj),
                 "{\"t\":\"state\",\"seq\":%u,\"st\":%u,\"thr\":%u,"
                 "\"alt\":%ld,\"spd\":%ld,\"max\":%ld,\"pa\":%ld,"
                 "\"ms\":%lu,\"fl\":%u,\"a1\":%u,\"a2\":%u}",
                 (unsigned)s->seq, (unsigned)s->state_id, (unsigned)s->thrust, (long)s->alt_cm, (long)s->speed_cms,
                 (long)s->max_alt_cm, (long)s->press_pa, (unsigned long)s->time_ms, (unsigned)s->flags,
                 (unsigned)s->p1_adc, (unsigned)s->p2_adc);
        json_send(obj);
    }
}

/* ── telemetry_apogee ─────────────────────────────────────────────── */

void telemetry_apogee(int32_t max_alt_cm, uint32_t flight_time_ms) {
    if (!s_cfg)
        return;

    if (s_cfg->telem_format == TELEM_FORMAT_NMEA) {
        char payload[80];
        snprintf(payload, sizeof(payload), "PYRO_APO,%ld,%lu", (long)max_alt_cm, (unsigned long)flight_time_ms);
        nmea_send(payload);

    } else if (s_cfg->telem_format == TELEM_FORMAT_JSON) {
        char obj[96];
        snprintf(obj, sizeof(obj), "{\"t\":\"apogee\",\"max\":%ld,\"ms\":%lu}", (long)max_alt_cm,
                 (unsigned long)flight_time_ms);
        json_send(obj);
    }
}

/* ── telemetry_pyro_fire ──────────────────────────────────────────── */

void telemetry_pyro_fire(uint8_t channel, int32_t alt_cm, uint32_t time_ms) {
    if (!s_cfg)
        return;

    if (s_cfg->telem_format == TELEM_FORMAT_NMEA) {
        char payload[80];
        snprintf(payload, sizeof(payload), "PYRO_FIRE,%u,%ld,%lu", (unsigned)channel, (long)alt_cm,
                 (unsigned long)time_ms);
        nmea_send(payload);

    } else if (s_cfg->telem_format == TELEM_FORMAT_JSON) {
        char obj[96];
        snprintf(obj, sizeof(obj), "{\"t\":\"fire\",\"ch\":%u,\"alt\":%ld,\"ms\":%lu}", (unsigned)channel, (long)alt_cm,
                 (unsigned long)time_ms);
        json_send(obj);
    }
}

/* ── telemetry_landing ────────────────────────────────────────────── */

void telemetry_landing(int32_t max_alt_cm, uint32_t flight_time_ms) {
    if (!s_cfg)
        return;

    if (s_cfg->telem_format == TELEM_FORMAT_NMEA) {
        char payload[80];
        snprintf(payload, sizeof(payload), "PYRO_LAND,%ld,%lu", (long)max_alt_cm, (unsigned long)flight_time_ms);
        nmea_send(payload);

    } else if (s_cfg->telem_format == TELEM_FORMAT_JSON) {
        char obj[96];
        snprintf(obj, sizeof(obj), "{\"t\":\"landing\",\"max\":%ld,\"ms\":%lu}", (long)max_alt_cm,
                 (unsigned long)flight_time_ms);
        json_send(obj);
    }
}

/* ── The state message, from the flight context ───────────────────── */

/* The internal state as the ground station knows it: spec §4 and §9. A state
 * the ground station has no code for (GROUND_TEST, through ground_test.c)
 * reports as the pad. */
static uint8_t state_to_telem_id(flight_state_t state) {
    switch (state) {
    case PAD_IDLE:
        return 0;
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

void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state) {
    telemetry_snapshot_t snap = {
        .seq = ctx->telemetry_seq,
        .state_id = state_to_telem_id(state),
        .thrust = (state == ASCENT && ctx->under_thrust) ? 1u : 0u,
        .alt_cm = altitude_cm,
        .max_alt_cm = ctx->max_altitude,
        .speed_cms = ctx->vertical_speed_cms,
        .press_pa = ctx->filtered_pressure,
        .p1_adc = ctx->pyro1_adc,
        .p2_adc = ctx->pyro2_adc,
        .time_ms = time_ms,
        .flags = 0,
    };
    if (ctx->pyro1_continuity_good)
        snap.flags |= TELEM_FLAG_P1_CONT;
    if (ctx->pyro2_continuity_good)
        snap.flags |= TELEM_FLAG_P2_CONT;
    if (ctx->pyro1_fired)
        snap.flags |= TELEM_FLAG_P1_FIRED;
    if (ctx->pyro2_fired)
        snap.flags |= TELEM_FLAG_P2_FIRED;
    if (ctx->pyros_armed)
        snap.flags |= TELEM_FLAG_ARMED;
    if (ctx->apogee_detected)
        snap.flags |= TELEM_FLAG_APOGEE;
    telemetry_state(&snap);
    ctx->telemetry_seq++;
}
