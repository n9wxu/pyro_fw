/*
 * Static validation of a Lua program against the configured resources.
 *
 * Answers one question before flight: is this script and this configuration a
 * matched pair? A script naming a UART the operator never assigned is a
 * mistake that would otherwise surface as a silent no-op at apogee.
 *
 * This is a READINESS check, not a security boundary. The sandbox in
 * pyro_lua.c is the security boundary, and it holds whatever a script
 * computes at runtime -- a name built by concatenation resolves through the
 * same lookup and gets the same refusal, and can no more reach pyro, the
 * pressure bus or the buzzer than a literal can. So this checker is free to
 * over-approximate, and does.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_CHECK_H
#define LUA_CHECK_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LUA_CHK_OK = 0,
    LUA_CHK_SYNTAX,  /* the chunk does not compile           */
    LUA_CHK_MISSING, /* names a resource config did not grant */
    LUA_CHK_UNKNOWN, /* names something no board provides     */
} lua_chk_kind_t;

typedef struct {
    lua_chk_kind_t kind;
    char detail[128];
} lua_chk_item_t;

#define LUA_CHK_MAX 8

typedef struct {
    int count;
    bool green; /* no SYNTAX and no MISSING; UNKNOWN is a warning */
    lua_chk_item_t items[LUA_CHK_MAX];
} lua_chk_result_t;

/* Compile the chunk and compare its string constants against the resources
 * the platform currently exposes. Does not execute a single line of it. */
void lua_check(const char *src, size_t len, lua_chk_result_t *out);

#endif
