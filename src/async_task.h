/*
 * Cooperative async task pattern for HAL hardware state machines.
 *
 * Each task has a scheduled next-run time and a tick function.
 * The HAL task runner (hal_tasks_tick) calls tick() when
 * now_ms >= next_due_ms.  tick() MUST update next_due_ms before
 * returning to reschedule itself.
 *
 * Usage:
 *   1. Define a struct with async_task_t as the FIRST member.
 *   2. Implement a tick() function that casts the base pointer back.
 *   3. Register via hw_task_register() at init time.
 *
 * Example:
 *   typedef struct {
 *       async_task_t base;   // MUST be first
 *       int my_state;
 *   } my_task_t;
 *
 *   static void my_tick(async_task_t *self, uint32_t now_ms) {
 *       my_task_t *t = (my_task_t *)self;
 *       // ... do work ...
 *       t->base.next_due_ms = now_ms + INTERVAL_MS;
 *   }
 *
 *   static my_task_t my_task = { .base = { .tick = my_tick } };
 *
 * Test/sim HALs implement hal_tasks_tick() as a no-op; the pattern
 * is transparent to all three HAL targets.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ASYNC_TASK_H
#define ASYNC_TASK_H

#include <stdint.h>

/*
 * A scheduled cooperative task.
 *
 * next_due_ms  When the task next wants to run (ms since boot).
 *              Set to 0 at declaration to run on the first tick.
 * tick         Called by hal_tasks_tick() when now_ms >= next_due_ms.
 *              NULL means the task is inactive / not yet started.
 */
typedef struct async_task {
    uint32_t next_due_ms;
    void (*tick)(struct async_task *self, uint32_t now_ms);
} async_task_t;

#endif /* ASYNC_TASK_H */
