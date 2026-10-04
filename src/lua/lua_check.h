/*
 * Static validation of a Lua program against the configured resources.
 *
 * Answers one question before flight: is this script and this configuration a
 * matched pair? A script naming a UART the operator never assigned would
 * otherwise surface as a silent no-op at apogee.
 *
 * A readiness check, not a security boundary: the sandbox in pyro_lua.c holds
 * whatever a script computes at runtime, so this checker is free to
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
 * Passed in rather than read from the platform: resources bind once at boot,
 * so between saving a configuration and rebooting into it the platform still
 * holds the previous one, and the question is whether the script will work
 * with the new one. */
#define LUA_CHK_MAX_NAMES 8

typedef struct {
    char names[LUA_CHK_MAX_NAMES][LUA_NAME_MAX];
    int n;
    bool has_output;
    bool has_input;
    bool has_serial;
    bool has_pixel;
} lua_chk_env_t;

/* What the platform has actually bound: the Lua task's start-up check. */
void lua_chk_env_from_platform(lua_chk_env_t *env);

typedef enum {
    LUA_CHK_OK = 0,
    LUA_CHK_SYNTAX,  /* the chunk does not compile, or memory ran out */
    LUA_CHK_MISSING, /* names a resource config did not grant         */
    LUA_CHK_UNKNOWN, /* names something no board provides             */
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

/* Compile the chunk and compare its string constants against env. Does not
 * execute a single line of it.
 *
 * lua_check() compiles in the checker's own static arena, so it has one
 * caller at a time: the net task. lua_check_in() takes the memory instead;
 * mem must be 8-aligned. Neither touches the system heap. */
void lua_check(const char *src, size_t len, const lua_chk_env_t *env, lua_chk_result_t *out);
void lua_check_in(void *mem, size_t mem_len, const char *src, size_t len, const lua_chk_env_t *env,
                  lua_chk_result_t *out);

#endif
