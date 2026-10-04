/*
 * MK1C's two measurements: the quiescent read, with no stimulus, and the
 * presence test, with the bus biased. See THEORY_OF_OPERATION.md
 * "Presence test".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_MEASURE_H
#define PYRO_MEASURE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t vbat, bus, a, b;
} quiescent_t;

typedef struct {
    uint16_t bus, a, b;
    bool valid;
} tracking_t;

void read_quiescent(quiescent_t *q);

void tracking_init(void);
bool tracking_step(uint32_t now_ms); /* true when a new result is in */
void tracking_abandon(void);         /* bias off; the test in progress is dropped */
bool tracking_pulse_active(void);    /* the bus is biased for a reading */
void tracking_run_next_at(uint32_t now_ms);
const tracking_t *tracking_result(void);

#endif
