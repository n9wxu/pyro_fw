/*
 * The flight log's ring, from the writer's side [FLT-LOG-07, DD-027].
 *
 * The ring goes to the file in pieces, and has each piece's room back as the
 * file takes it: the file stores a page in several periods, and the samples
 * of those periods need the room. The log's opening is written ahead of the
 * ring, so the ring counts as full while it still has room for the samples
 * that arrive behind the opening.
 *
 * Pure.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LOG_DRAIN_H
#define LOG_DRAIN_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_DRAIN_PIECE 128u
#define LOG_DRAIN_MARGIN 96u /* a text row, the longest record */

/* The next piece of a ring of `size` bytes, a power of two: its length, in
 * one run of the buffer from `tail`. */
static inline uint32_t log_drain_piece(uint32_t head, uint32_t tail, uint32_t size) {
    uint32_t off = tail & (size - 1u);
    uint32_t n = head - tail;
    if (n > size - off)
        n = size - off;
    return n < LOG_DRAIN_PIECE ? n : LOG_DRAIN_PIECE;
}

/* Whether the ring must be written now, with `opening` bytes to go out
 * ahead of it. Half the opening's size is kept free: samples arrive at a
 * fifth of the rate a growing file stores them, three programs a page. */
static inline bool log_drain_full(uint32_t used, uint32_t size, uint32_t opening) {
    uint32_t keep = LOG_DRAIN_MARGIN + opening / 2u;
    return keep >= size || used >= size - keep;
}

#endif
