/*
 * SPDX-License-Identifier: MIT
 */
#include "pyro_faults.h"
#include "pyro_sense.h"
#include <string.h>

#define AGREEING_SAMPLES 3          /* [PYR-CONT-04] See THEORY_OF_OPERATION.md "Short latches" */
#define PACK_PRESENT_MIN_COUNTS 200 /* about 1.5 V of pack: none fitted below it */

static struct {
    uint8_t latched;
    uint8_t hot_run, short_run;
} faults;

void faults_init(void) {
    memset(&faults, 0, sizeof(faults));
}

bool bus_is_hot(const quiescent_t *q) {
    return q->vbat > PACK_PRESENT_MIN_COUNTS && q->bus > (uint16_t)(q->vbat - q->vbat / 4u);
}

static void count_toward(bus_fault_t fault, uint8_t *run, bool seen) {
    *run = seen ? (uint8_t)(*run + 1) : 0;
    if (*run >= AGREEING_SAMPLES) {
        *run = AGREEING_SAMPLES;
        faults.latched |= fault;
    }
}

void faults_watch_bus_hot(const quiescent_t *q, bool bus_charged_by_sequence) {
    count_toward(FAULT_BUS_HOT, &faults.hot_run, !bus_charged_by_sequence && bus_is_hot(q));
}

/* One presence test is one sample, however many loops read it. */
void faults_count_bus_short(const tracking_t *t) {
    count_toward(FAULT_BUS_SHORTED, &faults.short_run, t->bus < TRACK_BUS_MIN_COUNTS);
}

void faults_latch(bus_fault_t fault) {
    faults.latched |= fault;
}

uint8_t faults_latched(void) {
    return faults.latched;
}
