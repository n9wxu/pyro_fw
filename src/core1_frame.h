/*
 * Core1's frame [FLT-LOG-09, LUA-RUN-04, DD-104].
 *
 * Each period core1 belongs first to the flight log's writer, for up to
 * FRAME_WRITER_US from the period's start; then to Lua's tick, for up to
 * FRAME_LUA_US, ending by FRAME_LUA_END_US. The rest of the period is spare.
 * The writer gives its slot up early when it has nothing more to do, and
 * Lua then starts at once.
 *
 * Times are the hardware timer's microseconds; t0 is the period's start,
 * the alarm's target, not the moment the flight task woke.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CORE1_FRAME_H
#define CORE1_FRAME_H

#include "loop_period.h"
#include <stdbool.h>
#include <stdint.h>

#define FRAME_WRITER_US 3000u
#define FRAME_LUA_US 6500u
#define FRAME_LUA_END_US 9500u

_Static_assert(FRAME_WRITER_US + FRAME_LUA_US <= FRAME_LUA_END_US, "Lua's slot follows the writer's");
_Static_assert(FRAME_LUA_END_US <= LOOP_PERIOD_US, "the frame is the period");

/* Microseconds of the writer's slot left at now; 0 once it has ended. */
static inline uint32_t frame_writer_left_us(uint32_t t0, uint32_t now) {
    uint32_t in = now - t0;
    return in < FRAME_WRITER_US ? FRAME_WRITER_US - in : 0u;
}

/* Lua's tick budget for a tick that starts at now; 0: too late this frame. */
static inline uint32_t frame_lua_budget_us(uint32_t t0, uint32_t now) {
    uint32_t in = now - t0;
    if (in >= FRAME_LUA_END_US)
        return 0u;
    uint32_t left = FRAME_LUA_END_US - in;
    return left < FRAME_LUA_US ? left : FRAME_LUA_US;
}

/* ── On the target (src/rtos/core1_frame.c) ───────────────────────── */

struct tskTaskControlBlock;

/* Before the scheduler starts: the tasks the frame wakes. */
void core1_frame_attach_storage(struct tskTaskControlBlock *task);
void core1_frame_attach_lua(struct tskTaskControlBlock *task);

/* The flight task, at the head of its period: the frame starts at t0, and
 * the storage task is woken for the writer's slot. Never blocks. */
void core1_frame_begin(uint32_t t0_us);

/* The start of the frame now running. */
uint32_t core1_frame_t0(void);

/* The writer's slot is given up for this frame; Lua may start. */
void core1_frame_writer_done(void);

/* The storage task: waits for the next frame, at most timeout_ms, and
 * returns whether one began. */
bool core1_frame_wait(uint32_t timeout_ms);

/* The Lua task, with a tick asked for frame t0: waits until the writer's
 * slot is over, and returns the tick's budget; 0 skips the tick. */
uint32_t core1_frame_lua_slot(uint32_t t0);

/* For /api/status: the writer's longest hold past t0, the times it ran past
 * its slot's end, and the ticks given no time. */
typedef struct {
    uint32_t writer_max_us;
    uint32_t writer_overruns;
    uint32_t lua_skipped;
} core1_frame_stats_t;
void core1_frame_stats(core1_frame_stats_t *out);

#endif
