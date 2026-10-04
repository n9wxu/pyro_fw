/*
 * Fixed-arena allocator for the Lua VM.
 *
 * Lua must not call malloc. The SDK guards malloc with a mutex whose
 * mutex_enter_blocking() has no timeout, so a Lua task stopped inside malloc
 * would leave every later caller waiting forever. Plan invariant L2:
 * thoughts/shared/plans/2026-09-21-lua-user-programs-core1.md
 *
 * So Lua gets its own heap: one static array, a first-fit list with
 * coalescing, and a hard ceiling. Exhaustion returns NULL, Lua raises "not
 * enough memory", the script dies, and nothing outside the arena notices.
 *
 * First-fit with coalescing suits Lua's pattern of many small short-lived
 * blocks and occasional large table or string reallocations, without a
 * size-class allocator's dependence on getting the classes right.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_arena.h"
#include <string.h>

/* `size` is the payload; next and prev are physical neighbours. */
typedef struct lua_arena_block {
    size_t size;
    struct lua_arena_block *next;
    struct lua_arena_block *prev;
    int free;
} block_t;

#define ALIGN 8u
#define ALIGN_UP(n) (((n) + (ALIGN - 1u)) & ~(ALIGN - 1u))
#define HDR ALIGN_UP(sizeof(block_t))
#define MIN_PAYLOAD 16u /* a smaller split remainder could never be reused */

void lua_arena_init(lua_arena_t *a, void *buf, size_t len) {
    memset(&a->stats, 0, sizeof(a->stats));
    a->head = (block_t *)buf;
    a->head->size = len - HDR;
    a->head->next = NULL;
    a->head->prev = NULL;
    a->head->free = 1;
    a->stats.total = len;
}

static void split(block_t *b, size_t want) {
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

static void absorb_next(block_t *b) {
    b->size += HDR + b->next->size;
    b->next = b->next->next;
    if (b->next)
        b->next->prev = b;
}

static void coalesce(block_t *b) {
    if (b->next && b->next->free)
        absorb_next(b);
    if (b->prev && b->prev->free) {
        b->prev->size += HDR + b->size;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

static void give_back_tail(block_t *b, size_t want) {
    size_t before = b->size;
    split(b, want);
    if (b->size != before && b->next->next && b->next->next->free)
        absorb_next(b->next);
}

static void *arena_malloc(lua_arena_t *a, size_t n) {
    size_t want = ALIGN_UP(n);
    for (block_t *b = a->head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            a->stats.in_use += b->size;
            if (a->stats.in_use > a->stats.peak)
                a->stats.peak = a->stats.in_use;
            a->stats.allocs++;
            return (uint8_t *)b + HDR;
        }
    }
    a->stats.failures++;
    return NULL;
}

static void arena_free(lua_arena_t *a, void *p) {
    if (!p)
        return;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    a->stats.in_use -= b->size;
    a->stats.frees++;
    b->free = 1;
    coalesce(b);
}

/* When ptr is NULL, osize is a type tag rather than a size (§4.6). */
void *lua_arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    lua_arena_t *a = ud;
    if (nsize == 0) {
        arena_free(a, ptr);
        return NULL;
    }
    if (ptr == NULL)
        return arena_malloc(a, nsize);

    block_t *b = (block_t *)((uint8_t *)ptr - HDR);
    size_t want = ALIGN_UP(nsize);
    size_t before = b->size;
    if (b->size < want && b->next && b->next->free && b->size + HDR + b->next->size >= want)
        absorb_next(b);
    if (b->size >= want) { /* never fails a shrink, as §4.6 requires */
        give_back_tail(b, want);
        a->stats.in_use += b->size - before; /* wraps down on a shrink */
        if (a->stats.in_use > a->stats.peak)
            a->stats.peak = a->stats.in_use;
        return ptr;
    }

    void *np = arena_malloc(a, nsize);
    if (!np)
        return NULL; /* the old block stays valid, as §4.6 requires */
    memcpy(np, ptr, osize < nsize ? osize : nsize);
    arena_free(a, ptr);
    return np;
}
