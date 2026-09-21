/*
 * Fixed-arena allocator for the Lua VM.
 *
 * Lua must not call malloc. On the target the VM runs on core1, and the SDK
 * guards malloc with a mutex whose mutex_enter_blocking() has no timeout — a
 * core1 that dies inside malloc would hang core0 permanently, which is the
 * one failure this whole design exists to prevent (invariant L2).
 *
 * So Lua gets its own heap: one static array, a first-fit free list with
 * coalescing, and a hard ceiling. Exhaustion returns NULL, Lua raises
 * "not enough memory", the script dies, and nothing outside this file
 * notices.
 *
 * The allocator is deliberately simple rather than clever. Lua's pattern is
 * many small short-lived blocks with occasional large table/string
 * reallocations, and first-fit with coalescing handles that without the
 * failure modes of a size-class allocator whose classes are wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_arena.h"
#include <string.h>

/* Block header. `size` is the payload; headers are never split below the
 * point where the remainder could not hold a header plus a minimum payload. */
typedef struct block {
    size_t size;
    struct block *next; /* physical next, not free-list next */
    struct block *prev;
    int free;
} block_t;

#define ALIGN 8u
#define ALIGN_UP(n) (((n) + (ALIGN - 1u)) & ~(ALIGN - 1u))
#define HDR ALIGN_UP(sizeof(block_t))
#define MIN_PAYLOAD 16u

static uint8_t *arena_base;
static size_t arena_size;
static block_t *arena_head;
static lua_arena_stats_t stats;

void lua_arena_init(void *buf, size_t len) {
    arena_base = (uint8_t *)buf;
    arena_size = len;
    memset(&stats, 0, sizeof(stats));

    arena_head = (block_t *)arena_base;
    arena_head->size = len - HDR;
    arena_head->next = NULL;
    arena_head->prev = NULL;
    arena_head->free = 1;

    stats.total = len;
}

static void split(block_t *b, size_t want) {
    /* Only split when the tail can hold a header and a usable payload,
     * otherwise the remainder becomes unreachable dead space. */
    if (b->size < want + HDR + MIN_PAYLOAD)
        return;
    block_t *tail = (block_t *)((uint8_t *)b + HDR + want);
    tail->size = b->size - want - HDR;
    tail->free = 1;
    tail->next = b->next;
    tail->prev = b;
    if (b->next)
        b->next->prev = tail;
    b->next = tail;
    b->size = want;
}

static void coalesce(block_t *b) {
    if (b->next && b->next->free) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

static void *arena_malloc(size_t n) {
    if (n == 0)
        return NULL;
    size_t want = ALIGN_UP(n);
    for (block_t *b = arena_head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            stats.in_use += b->size;
            if (stats.in_use > stats.peak)
                stats.peak = stats.in_use;
            stats.allocs++;
            return (uint8_t *)b + HDR;
        }
    }
    stats.failures++;
    return NULL;
}

static void arena_free(void *p) {
    if (!p)
        return;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    stats.in_use -= b->size;
    stats.frees++;
    b->free = 1;
    coalesce(b);
}

/* Lua's allocator contract: nsize == 0 frees; otherwise resize/allocate.
 * osize carries the old size (or a type tag when ptr is NULL). */
void *lua_arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        arena_free(ptr);
        return NULL;
    }
    if (ptr == NULL)
        return arena_malloc(nsize);

    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    if (b->size >= ALIGN_UP(nsize))
        return ptr; /* shrink or same bucket: keep it in place */

    void *np = arena_malloc(nsize);
    if (!np)
        return NULL; /* Lua will raise; the old block stays valid */
    memcpy(np, ptr, osize < nsize ? osize : nsize);
    arena_free(ptr);
    return np;
}

void lua_arena_get_stats(lua_arena_stats_t *out) {
    *out = stats;
}
