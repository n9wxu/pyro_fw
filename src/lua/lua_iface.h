/*
 * The resource table a script sees, and the interfaces behind it.
 *
 * A board publishes what it offers; Lua reaches it by name and kind through
 * one table of function pointers, so a board can expose a feature nothing
 * else has without the bindings learning about it.
 *
 * A wrong combination is not rejected, it is absent: lua_iface_find() takes
 * the kind it wants, so output.set() on a pad published as an input finds
 * nothing, because no output vtable was ever installed for it. Checks can be
 * forgotten; a missing pointer cannot be called. Invariants L4 and L5 of
 * thoughts/shared/plans/2026-09-21-lua-user-programs-core1.md
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_IFACE_H
#define LUA_IFACE_H

#include "lua_platform_cfg.h" /* LUA_NAME_MAX */
#include "pad_claim.h"
#include <stdbool.h>
#include <stdint.h>

/* Generous: the bound that bites is the board's pin count and PIO budget. */
#define LUA_IFACE_MAX 12

/* One kind per API shape. A script's `output`, `input`, `serial` and `pixel`
 * tables each resolve against exactly one of these. */
typedef enum {
    LUA_IF_NONE = 0, /* refused by publish and by lookup */
    LUA_IF_OUTPUT,
    LUA_IF_INPUT,
    LUA_IF_SERIAL,
    LUA_IF_PIXEL,
} lua_iface_kind_t;

/* ── The interfaces ───────────────────────────────────────────────
 *
 * One struct per kind, holding only the calls that kind supports. ctx is
 * whatever the publisher needs to identify the instance. */

typedef struct {
    void (*set)(void *ctx, int value); /* 0-100; 0/100 when not dimmable */
    int (*get)(void *ctx);
    bool dimmable;
} lua_if_output_t;

typedef struct {
    int (*get)(void *ctx);
} lua_if_input_t;

typedef struct {
    int (*write)(void *ctx, const char *s, int len);
    int (*read)(void *ctx, char *buf, int max);
} lua_if_serial_t;

typedef struct {
    int (*count)(void *ctx);
    void (*set)(void *ctx, int idx, uint8_t r, uint8_t g, uint8_t b);
    void (*show)(void *ctx);
} lua_if_pixel_t;

typedef struct {
    char name[LUA_NAME_MAX];
    lua_iface_kind_t kind;
    const void *vt; /* the struct for this kind, above */
    void *ctx;
    uint32_t pads; /* the pads it drives; see the publish rule below */
} lua_resource_t;

/* ── Publishing ───────────────────────────────────────────────────
 *
 * On core0, at boot, before the scheduler starts, like every other Lua
 * resource: the Lua task claims nothing.
 *
 * pads is every pad the resource drives, as a pad_claim.h mask, and the
 * publish claims all of them or fails having claimed none -- so a bridge
 * cannot be half-claimed, and a pad the flight software kept (it claims
 * first) can never appear here. There is one pad and one owner; nothing
 * compares two tables. A refused publish -- full table, duplicate name,
 * claim refused -- takes nothing.
 *
 * PAD_NONE is allowed, for a resource that drives no pad of its own.
 *
 * vt must have static storage: the table keeps the pointer, not a copy.
 * Returns the entry's index, or -1. */
int lua_iface_publish(uint32_t pads, const char *name, lua_iface_kind_t kind, const void *vt, void *ctx);

/* Drop every published resource. */
void lua_iface_reset(void);

/* ── Lookup ───────────────────────────────────────────────────────── */

/* The index of a resource published under this name AND this kind, or -1. */
int lua_iface_find(const char *name, lua_iface_kind_t kind);

const lua_resource_t *lua_iface_at(int idx);

/* How many of one kind are published, and the n'th of that kind: what the
 * list() bindings enumerate. */
int lua_iface_count_kind(lua_iface_kind_t kind);
const lua_resource_t *lua_iface_nth_of_kind(lua_iface_kind_t kind, int n);

/* ── Board hook ───────────────────────────────────────────────────
 *
 * Weak and empty: a board may implement it to publish a feature the shared
 * platform knows nothing about. Called from lua_plat_configure() once the
 * generic roles are in place. Not in board_if.h, which is the contract every
 * board must implement. */
void board_lua_publish(void);

#endif
