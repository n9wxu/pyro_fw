/*
 * The ground test switch, read once a loop [GND-TEST-12].
 *
 * Two wirings. A switch from one pad to ground: the pad is pulled up, and
 * low is closed. A switch across two pads, where a ground pad is hard to
 * reach: one pad is driven, alternately high and low, and the other is
 * pulled the opposite way each time. Closed, the read pad follows the drive
 * both ways; open, it follows its pull. A read pad touching ground or the
 * supply follows one way only, so it is never taken for a closed switch.
 *
 * The driven pad may be the buzzer's. The platform then shows the level under
 * test on it only for the instant of the read, so the buzzer keeps its own
 * pattern and the same two follows decide. The buzzer's pad is never the read
 * pad and never switched to ground.
 *
 * Portable: the platform moves the pins, this decides what they mean.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GROUND_TEST_SWITCH_H
#define GROUND_TEST_SWITCH_H

#include <stdbool.h>
#include <stdint.h>

/* How the switch is wired, as pins.ini's ground_test= names it. */
#define GT_WIRING_NONE 0u   /* no ground test switch */
#define GT_WIRING_GROUND 1u /* from the read pad to ground */
#define GT_WIRING_PAIR 2u   /* across the read pad and the driven pad */

typedef struct {
    bool pair;       /* across two pads, not to ground */
    bool drive_high; /* the driven pad's level since the last step */
    uint8_t follows; /* the last two reads, 1 where the read pad followed */
    bool asserted;
} gt_switch_t;

/* What the pins must be set to until the next step. */
typedef struct {
    bool drive_high; /* the driven pad (pair only) */
    bool pull_up;    /* the read pad's pull: up, or else down */
} gt_switch_out_t;

/* Start with the driven pad low and the read pad pulled up. */
void gt_switch_begin(gt_switch_t *w, bool pair);

/* Once a loop, with the read pad's level. */
gt_switch_out_t gt_switch_step(gt_switch_t *w, bool read_high);

bool gt_switch_asserted(const gt_switch_t *w);

#endif /* GROUND_TEST_SWITCH_H */
