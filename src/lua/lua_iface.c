/*
 * The resource table. See lua_iface.h for the argument.
 *
 * It stores what a publisher gives it and answers lookups; what may be
 * published is the pin assignment's decision, made before this runs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_iface.h"
#include <string.h>

static lua_resource_t table[LUA_IFACE_MAX];
static int n_res;

/* Weak and empty: most boards publish nothing of their own. */
__attribute__((weak)) void board_lua_publish(void) {}

void lua_iface_reset(void) {
    memset(table, 0, sizeof(table));
    n_res = 0;
}

int lua_iface_publish(uint32_t pads, const char *name, lua_iface_kind_t kind, const void *vt, void *ctx) {
    if (n_res >= LUA_IFACE_MAX || kind == LUA_IF_NONE || !vt) {
        return -1;
    }
    if (!name) {
        name = "";
    }

    /* Before the claim, so a refusal takes nothing. A duplicate would be
     * unreachable, because lookup resolves the first match; the pin
     * assignment refuses one, so this catches a publisher that invented a
     * name. */
    for (int i = 0; i < n_res; i++) {
        if (table[i].kind != LUA_IF_NONE && strcmp(table[i].name, name) == 0) {
            return -1;
        }
    }

    /* The claim is the permission. A pad the flight software kept cannot be
     * taken here, so no entry for it can be created -- see lua_iface.h. */
    if (!pad_claim_take(pads, PAD_LUA)) {
        return -1;
    }

    lua_resource_t *r = &table[n_res];
    strncpy(r->name, name, LUA_NAME_MAX - 1);
    r->name[LUA_NAME_MAX - 1] = '\0';
    r->kind = kind;
    r->vt = vt;
    r->ctx = ctx;
    r->pads = pads;
    return n_res++;
}

int lua_iface_find(const char *name, lua_iface_kind_t kind) {
    if (!name || kind == LUA_IF_NONE) {
        return -1;
    }
    for (int i = 0; i < n_res; i++) {
        if (table[i].kind == kind && strcmp(table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const lua_resource_t *lua_iface_at(int idx) {
    if (idx < 0 || idx >= n_res) {
        return NULL;
    }
    return &table[idx];
}

int lua_iface_count_kind(lua_iface_kind_t kind) {
    int n = 0;
    for (int i = 0; i < n_res; i++) {
        if (table[i].kind == kind) {
            n++;
        }
    }
    return n;
}

const lua_resource_t *lua_iface_nth_of_kind(lua_iface_kind_t kind, int n) {
    for (int i = 0; i < n_res; i++) {
        if (table[i].kind != kind) {
            continue;
        }
        if (n-- == 0) {
            return &table[i];
        }
    }
    return NULL;
}
