/*
 * The serial telemetry: a periodic state message and one message per event,
 * as $PYRO NMEA sentences or newline-delimited JSON (config.telem_format),
 * sent through hal_telemetry_send(). See
 * docs/ground-station-interface-spec.md for the protocol.
 *
 * The caller owns the sequence number: send_telemetry() keeps it in the
 * flight context.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_FORMATTER_H
#define TELEMETRY_FORMATTER_H

#include "config.h"
#include "flight_states.h"
#include <stdint.h>

/* ── telem_format values (config.telem_format) ───────────────────── */

#define TELEM_FORMAT_NMEA 0 /* the default */
#define TELEM_FORMAT_JSON 1

/* telemetry_snapshot_t.flags; spec §7.1. */

#define TELEM_FLAG_P1_CONT (1u << 0)  /* pyro 1 continuity good */
#define TELEM_FLAG_P2_CONT (1u << 1)  /* pyro 2 continuity good */
#define TELEM_FLAG_P1_FIRED (1u << 2) /* pyro 1 has fired       */
#define TELEM_FLAG_P2_FIRED (1u << 3) /* pyro 2 has fired       */
#define TELEM_FLAG_ARMED (1u << 4)    /* pyros armed            */
#define TELEM_FLAG_APOGEE (1u << 5)   /* apogee detected        */

typedef struct {
    uint16_t seq;
    uint8_t state_id; /* 0..5, state_to_telem_id(); spec §4 */
    uint8_t thrust;   /* 1 during powered ascent */
    uint8_t flags;    /* TELEM_FLAG_* */
    int32_t alt_cm;   /* above the ground reference */
    int32_t max_alt_cm;
    int32_t speed_cms; /* positive up */
    int32_t press_pa;  /* filtered */
    uint16_t p1_adc, p2_adc;
    uint32_t time_ms; /* since launch; 0 on the pad */
} telemetry_snapshot_t;

/* Borrows cfg, which must outlive telemetry. Before any other call. */
void telemetry_init(const config_t *cfg);

/* One state message, at cfg->telem_rate_hz. */
void telemetry_state(const telemetry_snapshot_t *s);

/* Each once per occurrence. */
void telemetry_apogee(int32_t max_alt_cm, uint32_t flight_time_ms);
void telemetry_pyro_fire(uint8_t channel, int32_t alt_cm, uint32_t time_ms);
void telemetry_landing(int32_t max_alt_cm, uint32_t flight_time_ms);

/* The internal state as the ground station knows it: see
 * docs/ground-station-interface-spec.md "4. Telemetry State Codes (the Fixed Contract)".
 * A state it has no code for (BOOT_*, FAULT, GROUND_TEST) reports as the pad. */
uint8_t state_to_telem_id(flight_state_t state);

#endif /* TELEMETRY_FORMATTER_H */
