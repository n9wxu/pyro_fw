/*
 * The resource table. See lua_iface.h for the argument.
 *
 * Deliberately dumb: it stores what a publisher gives it and answers lookups.
 * Every decision about what may be published belongs to the pin assignment,
 * which ran long before this did.
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

    /* The claim is the permission. A pad the flight software kept cannot be
     * taken here, so no entry for it can be created -- see lua_iface.h. */
    if (!pad_claim_take(pads, PAD_LUA)) {
        return -1;
    }

    /* A duplicate name would make the second resource unreachable, because
     * lookup resolves the first match. pin_assign_validate() already refuses
     * one, so reaching here means a publisher invented a name rather than
     * taking it from the assignment. */
    for (int i = 0; i < n_res; i++) {
        if (table[i].kind != LUA_IF_NONE && strcmp(table[i].name, name) == 0) {
            return -1;
        }
    }

    lua_resource_t *r = &table[n_res];
    strncpy(r->name, name ? name : "", LUA_NAME_MAX - 1);
    r->name[LUA_NAME_MAX - 1] = '\0';
    r->kind = kind;
    r->vt = vt;
    r->ctx = ctx;
    r->pads = pads;
    return n_res++;
}

void lua_iface_blank(int idx) {
    if (idx < 0 || idx >= n_res) {
        return;
    }
    /* Both the kind and the vtable, so neither a kind-matched lookup nor a
     * stale pointer can reach it. */
    table[idx].kind = LUA_IF_NONE;
    table[idx].vt = NULL;
    table[idx].ctx = NULL;
    table[idx].name[0] = '\0';
    /* The pads stay claimed. Blanking makes a resource unreachable from a
     * script; it does not hand a FET gate back to the flight software while
     * core1 may still be running. */
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

int lua_iface_count(void) {
    return n_res;
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
