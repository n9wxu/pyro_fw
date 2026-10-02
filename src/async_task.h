/*
 * A cooperative task a HAL state machine runs as. hal_tasks_tick() calls
 * tick() once now_ms reaches next_due_ms, and tick() sets next_due_ms before
 * it returns. A task embeds this as its first member and casts back in tick().
 * The test and sim HALs implement hal_tasks_tick() as a no-op.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ASYNC_TASK_H
#define ASYNC_TASK_H

#include <stdint.h>

typedef struct async_task {
    uint32_t next_due_ms; /* 0 runs on the first tick */
    void (*tick)(struct async_task *self, uint32_t now_ms); /* NULL: inactive */
} async_task_t;

#endif /* ASYNC_TASK_H */
