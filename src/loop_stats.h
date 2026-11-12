/*
 * The main loop's pacing figures and the last boot's record, kept by
 * main_hardware.c and hal_common.c for /api/status.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOOP_STATS_H
#define LOOP_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define STAGE_COUNT 9u
#define STAGE1_PART_COUNT 4u

/* Reported so the budget a board declares in board.cmake can be checked
 * against what it actually does, rather than being taken on trust. */
extern volatile uint32_t main_loop_count;
extern volatile uint32_t main_loop_max_us;
extern volatile uint32_t main_loop_overruns;
extern volatile uint32_t main_loop_overruns_ground;
extern volatile uint32_t main_loop_late_max_us;
extern volatile uint32_t main_loop_stage_max_us[STAGE_COUNT];
extern volatile uint32_t stage1_part_max_us[STAGE1_PART_COUNT]; /* TinyUSB, the stack, HTTP transport, mDNS */

extern volatile bool boot_prev_watchdog;
extern volatile int32_t boot_prev_stage;
extern volatile uint32_t boot_prev_stage_ms;

#endif
