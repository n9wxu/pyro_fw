/*
 * Is the flight code in RAM still the image's? [FLT-RT-08]
 *
 * The flight path runs from RAM (FLT-RT-02), where a stray write can change
 * it as flash cannot be changed. The image keeps the copy the start-up code
 * loaded it from, so the two are compared, a piece a call, round and round.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RAM_CHECK_H
#define RAM_CHECK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t at;
    uint32_t sweeps; /* whole comparisons finished */
    bool differs;    /* once seen, it stays */
} ram_check_t;

/* Compares the next `piece` bytes. True while no difference has been seen. */
bool ram_check_step(ram_check_t *c, const uint8_t *ram, const uint8_t *image, uint32_t len, uint32_t piece);

#endif
