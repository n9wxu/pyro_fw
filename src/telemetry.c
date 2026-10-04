/*
 * See telemetry.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "telemetry.h"
#include "hal.h"
#include <stdio.h>

static void send_sentence(const char *payload) {
    uint8_t checksum = 0;
    for (int i = 0; payload[i] != '\0'; i++)
        checksum ^= (uint8_t)payload[i];
    char sentence[220];
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, checksum);
    hal_telemetry_send(sentence);
}

static void send_event(const telemetry_event_t *e) {
    char payload[80];
    switch ((telemetry_event_kind_t)e->kind) {
    case TELEM_EVENT_APOGEE:
        snprintf(payload, sizeof(payload), "PYRO_APO,%ld,%lu", (long)e->alt_cm, (unsigned long)e->time_ms);
        break;
    case TELEM_EVENT_FIRE:
        snprintf(payload, sizeof(payload), "PYRO_FIRE,%u,%ld,%lu", (unsigned)e->channel, (long)e->alt_cm,
                 (unsigned long)e->time_ms);
        break;
    case TELEM_EVENT_LANDING:
        snprintf(payload, sizeof(payload), "PYRO_LAND,%ld,%lu", (long)e->alt_cm, (unsigned long)e->time_ms);
        break;
    default:
        return;
    }
    send_sentence(payload);
}

static void send_state(const telemetry_snapshot_t *s) {
    char payload[200];
    snprintf(payload, sizeof(payload), "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%d,%d", (unsigned)s->seq,
             (unsigned)s->state_id, (unsigned)s->thrust, (long)s->alt_cm, (long)s->speed_cms, (long)s->max_alt_cm,
             (long)s->press_pa, (unsigned long)s->time_ms, (unsigned)s->flags, (unsigned)s->p1_adc, (unsigned)s->p2_adc,
             0, 0);
    send_sentence(payload);
}

void telemetry_queue(telemetry_queue_t *q, telemetry_event_t event) {
    if (q->count >= TELEM_QUEUE_SIZE) {
        q->dropped++;
        return;
    }
    q->event[q->count++] = event;
}

void telemetry_send(telemetry_queue_t *q, const telemetry_snapshot_t *s) {
    for (uint8_t i = 0; i < q->count; i++)
        send_event(&q->event[i]);
    q->count = 0;
    send_state(s);
}
