/*
 * The ARM_TOGGLE charge pump: U9 stays enabled only while it toggles, and
 * stopping it is the disarm. A FIFO-paced PIO machine, fed once a loop. See
 * THEORY_OF_OPERATION.md "Arm pump".
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ARM_PUMP_H
#define ARM_PUMP_H

#define ARM_PUMP_PERIOD_US 100 /* 10 kHz: DESIGN.md 5.1's band is 10-50 */
#define ARM_PUMP_BURST 20      /* toggle cycles per pushed word = 2 ms      */

void arm_pump_init(void); /* at boot: the program and a state machine */
void arm_pump_start(void);
void arm_pump_feed(void); /* top the FIFO up; never waits */
void arm_pump_stop(void);

#endif /* ARM_PUMP_H */
