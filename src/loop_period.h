/*
 * The main loop's period [FLT-RT-04, DD-104]. Everything that paces itself
 * by the loop -- the pressure sensor's one-shot, MK1C's arm pump, the host
 * tests' model of the hardware -- takes it from here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOOP_PERIOD_H
#define LOOP_PERIOD_H

#define LOOP_PERIOD_MS 10u
#define LOOP_PERIOD_US (LOOP_PERIOD_MS * 1000u)

#endif
