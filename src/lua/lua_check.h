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

#include "lua_platform_cfg.h"
#include <stdbool.h>
#include <stddef.h>

/* The resource set to validate against.
 *
 * Passed in rather than read from the platform, because the two differ at
 * exactly the moment the check matters most: between saving a configuration
 * and rebooting into it. Resources bind once at boot, so the live platform
 * still reflects the PREVIOUS configuration, and a checker that read it would
 * tell the operator their new pin assignment was missing. The question is
 * "will this script and this configuration work together", so configuration
 * is the input. */
#define LUA_CHK_MAX_NAMES 8

typedef struct {
    char names[LUA_CHK_MAX_NAMES][LUA_NAME_MAX];
    int n;
    bool has_output;
    bool has_input;
    bool has_serial;
    bool has_pixel;
} lua_chk_env_t;

/* Describe what the platform has actually bound (used at boot). */
void lua_chk_env_from_platform(lua_chk_env_t *env);

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
void lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out);

#endif
