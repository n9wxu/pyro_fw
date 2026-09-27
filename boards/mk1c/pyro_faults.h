/*
 * MK1C's latched bus faults. See THEORY_OF_OPERATION.md "Short latches".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_FAULTS_H
#define PYRO_FAULTS_H

#include "pyro_measure.h"

typedef enum {
    FAULT_NONE = 0,
    FAULT_BUS_HOT = 1u << 0,          /* the bus at the pack, nothing armed */
    FAULT_BUS_SHORTED = 1u << 1,      /* the bus would not rise under bias  */
    FAULT_PRECHARGE_TIMEOUT = 1u << 2 /* the bus did not reach the pack     */
} bus_fault_t;

void faults_init(void);
bool bus_is_hot(const quiescent_t *q);
void faults_watch_bus_hot(const quiescent_t *q, bool bus_charged_by_sequence);
void faults_count_bus_short(const tracking_t *t);
void faults_latch(bus_fault_t fault);
uint8_t faults_latched(void);

#endif
