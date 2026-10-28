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

#include <stdint.h>

/* The period's alarm is never armed closer than this ahead of now. */
#define LOOP_ARM_MIN_US 200u

/* [FLT-RT-05] Periods a wake at now has passed since the one due at
 * target: the grid points after target that fall before now plus
 * LOOP_ARM_MIN_US. The alarm skips them, re-arming a period from now; each
 * is a period the flight task missed. A core whose interrupts were off
 * longer than a period lands here. The watchdog bounds the lateness far
 * inside 32 bits. */
static inline uint32_t loop_periods_skipped(uint64_t target_us, uint64_t now_us) {
    if (now_us + LOOP_ARM_MIN_US <= target_us + LOOP_PERIOD_US)
        return 0u;
    uint32_t late = (uint32_t)(now_us + LOOP_ARM_MIN_US - target_us);
    return (late - 1u) / LOOP_PERIOD_US;
}

#endif
