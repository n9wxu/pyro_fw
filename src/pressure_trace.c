/*
 * SPDX-License-Identifier: MIT
 */
#include "pressure_trace.h"
#include <string.h>

static ptrace_rec_t trace_ring[PTRACE_N];
/* The number the next record gets. Advanced only once the record is whole,
 * so a reader on another task sees whole records up to it. */
static volatile uint32_t next_seq;

void ptrace_reset(void) {
    (void)memset(trace_ring, 0, sizeof(trace_ring));
    next_seq = 0;
}

void ptrace_note(uint32_t at_us, uint32_t read_us, uint32_t raw, uint32_t raw_t, int32_t pa_c, ptrace_kind_t kind) {
    ptrace_rec_t *r = &trace_ring[next_seq % PTRACE_N];
    r->at_us = at_us;
    r->read_us = read_us;
    r->raw = raw;
    r->raw_t = raw_t;
    r->pa_c = pa_c;
    r->kind = (uint8_t)kind;
    __asm volatile("" ::: "memory");
    next_seq++;
}

static void put_trace_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int ptrace_read(uint32_t since, uint8_t *dst, int cap) {
    if (cap < 12) {
        return 0;
    }
    /* Read once: the writer moves it between any two reads. */
    uint32_t end = next_seq;
    uint32_t oldest = (end > PTRACE_N) ? (end - PTRACE_N) : 0u;
    uint32_t first = (since < oldest) ? oldest : ((since > end) ? end : since);
    uint32_t n = end - first;
    int32_t room_bytes = cap - 12;
    uint32_t room = (uint32_t)room_bytes / sizeof(ptrace_rec_t);
    if (n > room) {
        n = room;
    }
    (void)memcpy(dst, PTRACE_MAGIC, 4);
    put_trace_le32(&dst[4], first);
    put_trace_le32(&dst[8], first + n);
    uint8_t *o = &dst[12];
    for (uint32_t i = 0; i < n; i++) {
        const ptrace_rec_t *r = &trace_ring[(first + i) % PTRACE_N];
        put_trace_le32(o, r->at_us);
        put_trace_le32(&o[4], r->read_us);
        put_trace_le32(&o[8], r->raw);
        put_trace_le32(&o[12], r->raw_t);
        put_trace_le32(&o[16], (uint32_t)r->pa_c);
        o[20] = r->kind;
        o[23] = 0;
        o[22] = 0;
        o[21] = 0;
        o = &o[sizeof(ptrace_rec_t)];
    }
    uint32_t length = 12u + (n * (uint32_t)sizeof(ptrace_rec_t));
    return (int)length;
}
