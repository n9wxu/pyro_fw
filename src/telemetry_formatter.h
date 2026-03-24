/*
 * Telemetry formatter — protocol-independent event API.
 *
 * Flight software calls event functions (once per event) and the periodic
 * telemetry_state() function.  The formatter reads config.telem_format to
 * select the output protocol and calls hal_telemetry_send() as transport.
 *
 * Two formats are supported:
 *   0 — NMEA $PYRO sentences (default, backwards-compatible)
 *   1 — Newline-delimited JSON objects
 *
 * Adding a new format: add a TELEM_FORMAT_* constant and a new branch in
 * each telemetry_* function in telemetry_formatter.c.
 *
 * Sequence number: the caller owns the sequence counter and passes it in
 * telemetry_snapshot_t.seq.  flight_update_outputs() uses ctx->telemetry_seq
 * and increments it after each call.  This keeps the counter per-context so
 * existing unit tests (which create fresh flight_context_t{0} contexts) get
 * predictable sequence numbers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_FORMATTER_H
#define TELEMETRY_FORMATTER_H

#include "config.h"
#include <stdint.h>

/* ── telem_format values (config.telem_format) ───────────────────── */

#define TELEM_FORMAT_NMEA 0 /* $PYRO NMEA sentences  (default) */
#define TELEM_FORMAT_JSON 1 /* JSON objects, newline-delimited  */

/* ── Flags bits packed into telemetry_snapshot_t.flags ───────────── */

#define TELEM_FLAG_P1_CONT (1u << 0)  /* pyro 1 continuity good */
#define TELEM_FLAG_P2_CONT (1u << 1)  /* pyro 2 continuity good */
#define TELEM_FLAG_P1_FIRED (1u << 2) /* pyro 1 has fired       */
#define TELEM_FLAG_P2_FIRED (1u << 3) /* pyro 2 has fired       */
#define TELEM_FLAG_ARMED (1u << 4)    /* pyros armed            */
#define TELEM_FLAG_APOGEE (1u << 5)   /* apogee detected        */

/* ── Compact telemetry snapshot ──────────────────────────────────── */
/*
 * Built by flight software from flight_context_t fields.
 * The formatter never touches flight_context_t directly.
 * Caller manages seq — increment after each telemetry_state() call.
 */
typedef struct {
    uint16_t seq;       /* sequence number (caller manages)  */
    uint8_t state_id;   /* 0=pad 1=ascent 2=descent 3=landed */
    uint8_t thrust;     /* 1 during powered ascent           */
    uint8_t flags;      /* TELEM_FLAG_* bitmask              */
    int32_t alt_cm;     /* current altitude (cm AGL)         */
    int32_t max_alt_cm; /* peak altitude so far              */
    int32_t speed_cms;  /* vertical speed cm/s (+up)         */
    int32_t press_pa;   /* filtered pressure (Pa)            */
    uint16_t p1_adc;    /* pyro 1 raw ADC reading            */
    uint16_t p2_adc;    /* pyro 2 raw ADC reading            */
    uint32_t time_ms;   /* flight time ms (0 on pad)         */
} telemetry_snapshot_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time init — stores config pointer.
 * Must be called before any other telemetry_* function. */
void telemetry_init(const config_t *cfg);

/* Periodic state update — formats one message and sends it via
 * hal_telemetry_send().  Call at cfg->telem_rate_hz cadence.
 * Caller increments snap->seq after the call. */
void telemetry_state(const telemetry_snapshot_t *s);

/* Event messages — called exactly once per occurrence.
 * Both formats emit a dedicated sentence for each event. */
void telemetry_apogee(int32_t max_alt_cm, uint32_t flight_time_ms);
void telemetry_pyro_fire(uint8_t channel, int32_t alt_cm, uint32_t time_ms);
void telemetry_landing(int32_t max_alt_cm, uint32_t flight_time_ms);

#endif /* TELEMETRY_FORMATTER_H */
