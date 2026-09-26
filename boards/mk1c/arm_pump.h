/*
 * ARM_TOGGLE charge pump — Pyro MK1C.
 *
 * The firing bus is energised only while U9 is enabled, and U9's enable is
 * held up only while ARM_TOGGLE keeps toggling (DESIGN.md 5.1, invariant 5).
 * arm_pump.pio does the toggling, a burst for each word pushed, and stalls
 * when the words run out. Stopping is the disarm: C_HOLD bleeds and U9
 * turns off in about 9.6 ms.
 *
 * The firing sequence feeds it once a loop, from the code path that has just
 * re-checked the arm conditions. The FIFO's four words and the one running
 * hold 5 x ARM_PUMP_BURST cycles, 10 ms: enough to carry the pump across one
 * loop period, and all it can run past the last check. A loop that stops
 * feeding it has the bus disarmed within about 20 ms [DD-056].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ARM_PUMP_H
#define ARM_PUMP_H

#define ARM_PUMP_PERIOD_US 100 /* 10 kHz, bottom of the DESIGN.md 5.1 band */
#define ARM_PUMP_BURST 20      /* toggle cycles per pushed word = 2 ms      */

void arm_pump_init(void); /* at boot: the program and a state machine */
void arm_pump_start(void);
void arm_pump_feed(void); /* top the FIFO up; never waits */
void arm_pump_stop(void);

#endif /* ARM_PUMP_H */
