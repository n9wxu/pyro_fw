/*
 * SPDX-License-Identifier: MIT
 */
#include "pressure_trace.h"
#include <string.h>

static ptrace_rec_t ring[PTRACE_N];
static uint32_t next_seq; /* the number the next record gets */

void ptrace_reset(void) {
    memset(ring, 0, sizeof(ring));
    next_seq = 0;
}

void ptrace_note(uint32_t at_us, uint32_t read_us, uint32_t raw, uint32_t raw_t, int32_t pa_c, ptrace_kind_t kind) {
    ptrace_rec_t *r = &ring[next_seq % PTRACE_N];
    r->at_us = at_us;
    r->read_us = read_us;
    r->raw = raw;
    r->raw_t = raw_t;
    r->pa_c = pa_c;
    r->kind = (uint8_t)kind;
    next_seq++;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int ptrace_read(uint32_t since, uint8_t *dst, int cap) {
    if (cap < 12) {
        return 0;
    }
    uint32_t oldest = next_seq > PTRACE_N ? next_seq - PTRACE_N : 0;
    uint32_t first = since < oldest ? oldest : since > next_seq ? next_seq : since;
    uint32_t n = next_seq - first;
    uint32_t room = (uint32_t)(cap - 12) / sizeof(ptrace_rec_t);
    if (n > room) {
        n = room;
    }
    memcpy(dst, PTRACE_MAGIC, 4);
    put32(dst + 4, first);
    put32(dst + 8, first + n < next_seq ? first + n : next_seq);
    uint8_t *o = dst + 12;
    for (uint32_t i = 0; i < n; i++) {
        const ptrace_rec_t *r = &ring[(first + i) % PTRACE_N];
        put32(o, r->at_us);
        put32(o + 4, r->read_us);
        put32(o + 8, r->raw);
        put32(o + 12, r->raw_t);
        put32(o + 16, (uint32_t)r->pa_c);
        o[20] = r->kind;
        o[21] = o[22] = o[23] = 0;
        o += sizeof(ptrace_rec_t);
    }
    return (int)(o - dst);
}
