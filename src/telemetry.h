/*
 * The $PYRO downlink [TEL-01..12, DD-088]. The sentence formats are in
 * docs/ground-station-interface-spec.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

#define TELEM_FLAG_P1_READY (1u << 0)
#define TELEM_FLAG_P2_READY (1u << 1)
#define TELEM_FLAG_P1_FIRED (1u << 2)
#define TELEM_FLAG_P2_FIRED (1u << 3)
#define TELEM_FLAG_ARMED (1u << 4)
#define TELEM_FLAG_APOGEE (1u << 5)

typedef struct {
    uint16_t seq;
    uint8_t state_id; /* [TEL-07] */
    uint8_t thrust;
    uint8_t flags;
    int32_t alt_cm, max_alt_cm, speed_cms, press_pa;
    uint16_t p1_adc, p2_adc;
    uint32_t time_ms;
} telemetry_snapshot_t;

typedef enum { TELEM_EVENT_APOGEE, TELEM_EVENT_FIRE, TELEM_EVENT_LANDING } telemetry_event_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t channel; /* TELEM_EVENT_FIRE */
    int32_t alt_cm;  /* the peak for apogee and landing; the height for a fire */
    uint32_t time_ms;
} telemetry_event_t;

/* Events wait here for the next message [TEL-11]. */
#define TELEM_QUEUE_SIZE 12
typedef struct {
    telemetry_event_t event[TELEM_QUEUE_SIZE];
    uint8_t count;
    uint32_t dropped;
} telemetry_queue_t;

void telemetry_queue(telemetry_queue_t *q, telemetry_event_t event);

/* One message: the queued events, oldest first, then the state sentence. */
void telemetry_send(telemetry_queue_t *q, const telemetry_snapshot_t *s);

#endif
