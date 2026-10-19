/*
 * A flash operation inside the log writer's slots [FLT-LOG-09, FLT-LOG-10,
 * DD-104].
 *
 * From launch to landing the writer runs a page program or a sector erase
 * on core1 only, in pieces that each end inside its slot: a piece starts
 * only when, by the chip's datasheet (flash_timing.h), it will end or can
 * be suspended before the slot does. A piece still running at its suspend
 * point is suspended, the rest of the frame is Lua's, and the next slot
 * resumes it. Between pieces the flash reads, so code on core1 runs from it.
 *
 * A piece that waits for a slot starts at the slot's start, wherever the
 * writer's task is then: core1's other tasks share its priority, and its
 * task can wake a millisecond into the slot.
 *
 * A part the table does not list gives nothing to plan by: its program
 * waits for the next slot and runs whole from its start, and its erase is
 * refused until landing.
 *
 * The sequencing here is plain C over the io below; the target's io runs
 * each piece from RAM with core1's interrupts off (flash_op.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_SLOT_H
#define FLASH_SLOT_H

#include "flash_timing.h"
#include <stdbool.h>
#include <stdint.h>

/* What a piece costs beside the chip's own time, measured on MK1C: each
 * command through the SDK's flash_do_cmd() hands XIP back, about 100 us.
 * A start is the write enable and the command, and the end is seen a poll
 * late; a suspend is the poll that sees its time, the command, its settle
 * and the polls until the chip reads idle. */
#define FLASH_SLOT_START_US 300u
#define FLASH_SLOT_SUSPEND_US 500u

/* Slots one operation may take: the slowest listed erase, 800 ms, over the
 * slot's useful part, with room to spare. Past it the operation is given up. */
#define FLASH_SLOT_MAX_PIECES 2000u

typedef enum { SLOT_PROGRAM, SLOT_ERASE } flash_slot_kind_t;

/* One piece: its command, and when to suspend it if it is still running. */
typedef struct {
    bool resume;           /* resume a suspended operation; else start it */
    bool may_suspend;      /* else run it to its end                      */
    uint32_t suspend_at;   /* absolute: suspend from here if still running */
    uint32_t suspend_from; /* absolute: never earlier than this (tRS)      */
} flash_piece_t;

/* Whether a piece may start at now in a slot that ends at slot_end, and if
 * so how it runs. */
bool flash_slot_plan(const flash_timing_t *t, flash_slot_kind_t kind, bool resume, uint32_t now, uint32_t slot_end,
                     flash_piece_t *out);

/* <0: the chip or the bus failed. FLASH_SLOT_NOT_YET: no piece fits the slot. */
enum { PIECE_DONE = 0, PIECE_SUSPENDED = 1, FLASH_SLOT_NOT_YET = 2 };

struct flash_slot_io;

/* An operation, and how far it has gone. t NULL: a part the table does not
 * list, whose piece runs whole. */
typedef struct {
    const struct flash_slot_io *io;
    const flash_timing_t *t;
    flash_slot_kind_t kind;
    bool resume; /* a piece of it has been suspended */
    uint32_t addr;
    const uint8_t *data;
    uint32_t len;
} flash_slot_req_t;

typedef struct flash_slot_io {
    void *ctx;
    uint32_t (*now_us)(void *ctx);
    uint32_t (*slot_t0)(void *ctx); /* the start of the frame now running */
    /* The slot is given up: flash_slot_try(r, its start) runs at the next
     * slot's start, and what it returned comes back. */
    int (*at_slot_start)(void *ctx, const flash_slot_req_t *r);
    bool (*in_flight)(void *ctx); /* false: the rest runs whole, under the lockout */
    int (*piece)(void *ctx, flash_slot_kind_t kind, uint32_t addr, const uint8_t *data, uint32_t len,
                 const flash_piece_t *p);
    int (*whole)(void *ctx, flash_slot_kind_t kind, bool resume, uint32_t addr, const uint8_t *data, uint32_t len);
} flash_slot_io_t;

/* The next piece, now, in the slot that started at slot_t0, if it fits:
 * what the piece returned, or FLASH_SLOT_NOT_YET. */
int flash_slot_try(const flash_slot_req_t *r, uint32_t slot_t0);

/* Runs one operation to its end: 0; <0 as a piece or the whole failed;
 * FLASH_SLOT_ETIME past FLASH_SLOT_MAX_PIECES; FLASH_SLOT_EREFUSED for an
 * erase no slot can hold. t NULL: a part the table does not list. */
int flash_slot_run(const flash_slot_io_t *io, const flash_timing_t *t, flash_slot_kind_t kind, uint32_t addr,
                   const uint8_t *data, uint32_t len);

#define FLASH_SLOT_ETIME (-62)
#define FLASH_SLOT_EREFUSED (-63)

#endif
