/*
 * A flash operation inside the log writer's slots. See flash_slot.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_slot.h"
#include "core1_frame.h"

#include <stddef.h>

static bool before_or_at(uint32_t a, uint32_t b) {
    return (int32_t)(b - a) >= 0;
}

bool flash_slot_plan(const flash_timing_t *t, flash_slot_kind_t kind, bool resume, uint32_t now, uint32_t slot_end,
                     flash_piece_t *out) {
    bool suspends = kind == SLOT_ERASE ? t->erase_suspends : t->program_suspends;
    out->resume = resume;
    out->may_suspend = suspends;
    if (suspends) {
        out->suspend_from = now + FLASH_SLOT_START_US + t->resume_hold_us;
        out->suspend_at = slot_end - t->suspend_us - FLASH_SLOT_SUSPEND_US;
        return before_or_at(out->suspend_from, out->suspend_at);
    }
    out->suspend_from = out->suspend_at = 0;
    return kind == SLOT_PROGRAM && before_or_at(now + FLASH_SLOT_START_US + t->program_max_us, slot_end);
}

int flash_slot_try(const flash_slot_req_t *r, uint32_t slot_t0) {
    const flash_slot_io_t *io = r->io;
    flash_piece_t p = {r->resume, false, 0, 0};
    if (r->t && !flash_slot_plan(r->t, r->kind, r->resume, io->now_us(io->ctx), slot_t0 + FRAME_WRITER_US, &p))
        return FLASH_SLOT_NOT_YET;
    return io->piece(io->ctx, r->kind, r->addr, r->data, r->len, &p);
}

/* The first piece tries the slot it is asked in. A part with no figures
 * waits for a slot's start, where its program runs whole. */
int flash_slot_run(const flash_slot_io_t *io, const flash_timing_t *t, flash_slot_kind_t kind, uint32_t addr,
                   const uint8_t *data, uint32_t len) {
    flash_slot_req_t r = {io, t, kind, false, addr, data, len};
    for (uint32_t n = 0; n < FLASH_SLOT_MAX_PIECES; n++) {
        if (!io->in_flight(io->ctx))
            return io->whole(io->ctx, kind, r.resume, addr, data, len);
        if (!t && kind == SLOT_ERASE)
            return FLASH_SLOT_EREFUSED;
        int rc = (n == 0 && t) ? flash_slot_try(&r, io->slot_t0(io->ctx)) : io->at_slot_start(io->ctx, &r);
        if (rc == PIECE_SUSPENDED) {
            r.resume = true;
            continue;
        }
        if (rc != FLASH_SLOT_NOT_YET)
            return rc;
        if (!r.resume && kind == SLOT_ERASE && !t->erase_suspends)
            return FLASH_SLOT_EREFUSED;
    }
    return FLASH_SLOT_ETIME;
}
