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
        out->suspend_from = now + t->resume_hold_us;
        out->suspend_at = slot_end - t->suspend_us - FLASH_SLOT_MARGIN_US;
        return before_or_at(out->suspend_from, out->suspend_at);
    }
    out->suspend_from = out->suspend_at = 0;
    return kind == SLOT_PROGRAM && before_or_at(now + t->program_max_us + FLASH_SLOT_MARGIN_US, slot_end);
}

/* A part with no figures: its program alone, whole, from a slot's start. */
static int unplanned(const flash_slot_io_t *io, flash_slot_kind_t kind, uint32_t addr, const uint8_t *data,
                     uint32_t len) {
    if (kind == SLOT_ERASE)
        return FLASH_SLOT_EREFUSED;
    io->next_slot(io->ctx);
    if (!io->in_flight(io->ctx))
        return io->whole(io->ctx, kind, false, addr, data, len);
    flash_piece_t p = {false, false, 0, 0};
    return io->piece(io->ctx, kind, addr, data, len, &p);
}

int flash_slot_run(const flash_slot_io_t *io, const flash_timing_t *t, flash_slot_kind_t kind, uint32_t addr,
                   const uint8_t *data, uint32_t len) {
    bool started = false;
    for (uint32_t n = 0; n < FLASH_SLOT_MAX_PIECES; n++) {
        if (!io->in_flight(io->ctx))
            return io->whole(io->ctx, kind, started, addr, data, len);
        if (!t)
            return unplanned(io, kind, addr, data, len);
        flash_piece_t p;
        uint32_t end = io->slot_t0(io->ctx) + FRAME_WRITER_US;
        if (!flash_slot_plan(t, kind, started, io->now_us(io->ctx), end, &p)) {
            if (!started && kind == SLOT_ERASE && !t->erase_suspends)
                return FLASH_SLOT_EREFUSED;
            io->next_slot(io->ctx);
            continue;
        }
        int rc = io->piece(io->ctx, kind, addr, data, len, &p);
        if (rc != PIECE_SUSPENDED)
            return rc;
        started = true;
        io->next_slot(io->ctx);
    }
    return FLASH_SLOT_ETIME;
}
