/*
 * The resource table a script sees, and the interfaces behind it.
 *
 * A board publishes what it offers; Lua reaches it by name and kind. Between
 * those two sits one table of function pointers, so a board can expose a
 * feature nothing else has without the Lua bindings learning about it, and
 * adding an interface costs a vtable rather than a new array, a new count, a
 * new find_ and a new binding in four files.
 *
 * SAFETY BY REACHABILITY
 *
 * A wrong combination is not rejected, it is absent. lua_iface_find() takes
 * the kind it wants, so output.set() on a pad published as an input finds
 * nothing: that entry carries an input vtable and no output vtable was ever
 * installed. A blanked entry carries no vtable at all and every lookup
 * misses it.
 *
 * That is the same argument as invariant L4 -- a capability configuration did
 * not enable has no table, so a script referencing it fails legibly -- and
 * the same as the half-bridge PIO program, where shoot-through is not avoided
 * but unencodable. Checks can be forgotten; a missing pointer cannot be
 * called.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_IFACE_H
#define LUA_IFACE_H

#include "lua_platform_cfg.h" /* LUA_NAME_MAX */
#include "pad_claim.h"
#include <stdbool.h>
#include <stdint.h>

/* How many resources a script can be given at once. Generous: the bound that
 * actually bites is the board's pin count and the PIO budget. */
#define LUA_IFACE_MAX 12

/* One kind per API shape. A script's `output`, `input`, `serial` and `pixel`
 * tables each resolve against exactly one of these. */
typedef enum {
    LUA_IF_NONE = 0, /* blank: unreachable from Lua */
    LUA_IF_OUTPUT,
    LUA_IF_INPUT,
    LUA_IF_SERIAL,
    LUA_IF_PIXEL,
} lua_iface_kind_t;

/* ── The interfaces ───────────────────────────────────────────────
 *
 * Separate structs rather than one wide one, so a pixel vtable cannot be
 * reached through an output lookup even by mistake: the kind selects the
 * struct, and the struct has only the calls that kind supports.
 *
 * ctx is whatever the publisher needs to identify the instance. Nothing here
 * interprets it. */

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
    const void *vt; /* one of the structs above; NULL when blank */
    void *ctx;
    uint32_t pads; /* the pads it drives; see the publish rule below */
} lua_resource_t;

/* ── Publishing ───────────────────────────────────────────────────
 *
 * Called on core0, at boot, before core1 exists -- the same rule every other
 * Lua resource follows, because claiming hardware later means taking a spin
 * lock on a core that must never hold one.
 *
 * pads is every pad the resource drives, as a pad_claim.h mask. The publish
 * FAILS unless Lua can claim all of them, and claims them when it can.
 *
 * That is what makes it impossible for a pad to be here and in the pyro table
 * at once: the flight software spends its claims first, so a pad it kept
 * cannot be claimed here, and the entry cannot exist without the claim.
 * Nothing compares the two tables -- there is one pad and one owner.
 *
 * All or nothing, which is what stops a half-claimed bridge existing: a
 * resource driving two pads takes both or neither.
 *
 * PAD_NONE is allowed, for a resource that drives no pad of its own; it
 * cannot conflict over one.
 *
 * vt must have static storage: the table keeps the pointer, not a copy. */
int lua_iface_publish(uint32_t pads, const char *name, lua_iface_kind_t kind, const void *vt, void *ctx);

/* Drop every published resource. */
void lua_iface_reset(void);

/* Make one entry unreachable: kind NONE, no vtable. A script holding its name
 * stops resolving rather than reaching a half-configured resource. */
void lua_iface_blank(int idx);

/* ── Lookup ───────────────────────────────────────────────────────── */

/* The index of a resource published under this name AND this kind, or -1.
 *
 * Taking the kind is what makes a wrong combination unreachable rather than
 * merely refused -- see the file comment. */
int lua_iface_find(const char *name, lua_iface_kind_t kind);

int lua_iface_count(void);
const lua_resource_t *lua_iface_at(int idx);

/* How many of one kind are published, and the n'th of that kind. What the
 * `list()` bindings enumerate. */
int lua_iface_count_kind(lua_iface_kind_t kind);
const lua_resource_t *lua_iface_nth_of_kind(lua_iface_kind_t kind, int n);

/* ── Board hook ───────────────────────────────────────────────────
 *
 * Weak and empty. A board implements it to publish a feature the shared
 * platform knows nothing about, using lua_iface_publish(). Called from
 * lua_plat_configure() once the generic roles are in place, so a board can
 * see what is already published and add to it.
 *
 * Here rather than in board_if.h because board_if.h is the contract every
 * board must implement, and this is an option a board may take up. */
void board_lua_publish(void);

#endif
