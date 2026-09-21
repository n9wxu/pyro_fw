/*
 * Fixed-arena allocator for the Lua VM. See lua_arena.c for why Lua may not
 * use malloc.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_ARENA_H
#define LUA_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t total;    /* arena bytes                          */
    size_t in_use;   /* payload bytes currently handed out   */
    size_t peak;     /* high-water mark of in_use            */
    uint32_t allocs; /* successful allocations               */
    uint32_t frees;
    uint32_t failures; /* allocations refused: the arena cap  */
} lua_arena_stats_t;

void lua_arena_init(void *buf, size_t len);

/* Matches lua_Alloc so it can be handed straight to lua_newstate(). */
void *lua_arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

void lua_arena_get_stats(lua_arena_stats_t *out);

#endif
