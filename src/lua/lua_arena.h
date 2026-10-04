/*
 * Fixed-arena allocator for the Lua VM and the script checker. See
 * lua_arena.c for why Lua may not use malloc.
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

struct lua_arena_block;

typedef struct {
    struct lua_arena_block *head;
    lua_arena_stats_t stats;
} lua_arena_t;

/* buf must be 8-aligned: blocks are aligned relative to it. */
void lua_arena_init(lua_arena_t *a, void *buf, size_t len);

/* A lua_Alloc (Lua 5.4 manual §4.6); ud is the lua_arena_t. */
void *lua_arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

#endif
