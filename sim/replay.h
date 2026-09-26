/*
 * Replaying a flight log through the firmware.
 *
 * A log carries each sample's own raw reading (raw_pa): the middle reading of
 * the median window that produced the sample. So the rows are the flight's
 * readings, one each, in order, and feeding them back through the pressure
 * layer reproduces the same medians and the same filter. The detectors then
 * decide again, and their decisions can be set against the log's own event
 * rows -- which is how a change to the filter or the detectors is checked
 * against a real flight [DAT-02].
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

/* The events the firmware decides from the log's readings, starting in ASCENT
 * with the log's configuration and ground pressure. false if the log has no
 * raw_pa column. */
bool replay_run(const char *csv, replay_events_t *out);

/* Called before each row with its time: what a HAL's own tick would do, such
 * as end a pyro pulse. NULL for none. */
extern void (*replay_row_hook)(uint32_t now_ms);

#endif
