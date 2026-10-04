/*
 * Replaying a flight log through the firmware [DAT-08].
 *
 * A log written with log_rate=full carries each sample's raw reading, so the
 * rows are the flight's readings in order. The firmware is started on a pad
 * at the log's ground pressure and then flown on those readings, and what it
 * decides is set against the log's own event rows: how a change to the
 * estimator or the detectors is checked against a real flight.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef REPLAY_H
#define REPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Times since T+0; 0 where the event is absent. */
typedef struct {
    uint32_t apogee_ms, pyro1_ms, pyro2_ms, landing_ms;
    int rows;
    uint32_t diverged_ms; /* the first sample row whose logged state the replay did not reach; 0: none */
} replay_events_t;

/* The events the log itself records. */
bool replay_logged_events(const char *csv, replay_events_t *out);

/* The events the firmware decides from the log's readings, with the log's
 * configuration and ground pressure. false if the log has no raw_pa column,
 * or was written at a row a second. */
bool replay_run(const char *csv, replay_events_t *out);

/* Called before each reading with its time, to set the HAL's clock and do
 * what its own tick would do, such as end a pyro pulse. */
extern void (*replay_row_hook)(uint32_t now_ms);

#endif
