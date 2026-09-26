/*
 * ARM_TOGGLE charge pump — Pyro MK1C.
 *
 * The firing bus is energised only while U9 is enabled, and U9's enable is
 * held up only while ARM_TOGGLE keeps toggling (DESIGN.md 5.1, invariant 5).
 * arm_pump.pio does the toggling, a burst for each word pushed, and stalls
 * when the words run out; these start and stop it. Stopping is the disarm:
 * C_HOLD bleeds and U9 turns off in about 9.6 ms.
 *
 * For the firing path (docs/outstanding_tasks.md, F1), which will feed the
 * pump from the loop. Nothing calls it yet (DD-055).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ARM_PUMP_H
#define ARM_PUMP_H

#define ARM_PUMP_PERIOD_US 100 /* 10 kHz, bottom of the DESIGN.md 5.1 band */
#define ARM_PUMP_BURST 10      /* toggle cycles per pushed word = 1 ms      */

void arm_pump_start(void);
void arm_pump_stop(void);

#endif /* ARM_PUMP_H */
