/*
 * The main loop's period [FLT-RATE-01, DD-065]. Everything that paces itself
 * by the loop -- the pressure sensor's one-shot, MK1C's arm pump, the host
 * tests' model of the hardware -- takes it from here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOOP_PERIOD_H
#define LOOP_PERIOD_H

#define LOOP_PERIOD_MS 20u
#define LOOP_PERIOD_US (LOOP_PERIOD_MS * 1000u)

#endif
