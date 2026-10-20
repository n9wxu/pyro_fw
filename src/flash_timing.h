/*
 * What the boot flash's datasheet promises about time, by its JEDEC ID
 * [FLT-LOG-09, FLT-LOG-10, DD-104].
 *
 * The log writer starts a flash operation only when, by these figures, it
 * ends or can be suspended inside the writer's slot. Every figure is the
 * datasheet's worst case over the part's voltage and temperature range.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_TIMING_H
#define FLASH_TIMING_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *part;
    uint32_t program_max_us; /* tPP: one page program                         */
    uint32_t erase_max_us;   /* tSE: one 4 kB sector erase                    */
    uint32_t suspend_us;     /* tSUS, tESL: suspend to the next read          */
    uint32_t resume_hold_us; /* tRS, tERS: a start or resume to its suspend   */
    bool program_suspends;   /* 75h suspends a page program                   */
    bool erase_suspends;     /* 75h suspends a sector erase                   */
} flash_timing_t;

/* The part's figures; NULL for a part not in the table, which promises
 * nothing the writer could plan by. */
const flash_timing_t *flash_timing_for(const uint8_t jedec[3]);

#endif
