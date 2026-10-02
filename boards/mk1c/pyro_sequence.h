/*
 * MK1C's firing sequence, a step a loop [PYR-ARM-01..06].
 * See THEORY_OF_OPERATION.md "Firing sequence".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_SEQUENCE_H
#define PYRO_SEQUENCE_H

#include "pyro_measure.h"

typedef enum {
    STEP_IDLE,      /* the presence test runs              */
    STEP_PRECHARGE, /* the pump runs; the bus ramps        */
    STEP_HOLD,      /* the gate on; the pump stopped       */
    STEP_DRAIN,     /* the gate open; the bleed drains it  */
} fire_step_t;

void sequence_init(void);
const char *sequence_refusal(uint8_t channel, const quiescent_t *q, const tracking_t *t);
void sequence_arm(uint8_t channel, const quiescent_t *q);
void sequence_step(uint32_t now_ms, const quiescent_t *q);
fire_step_t sequence_current_step(void);
bool sequence_firing(void);
bool sequence_charged_the_bus(uint32_t now_ms);
bool sequence_fired_since_tracking(uint8_t channel);
void sequence_verify_fired(const tracking_t *t);

#endif
