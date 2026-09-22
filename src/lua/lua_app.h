/*
 * Application glue for Lua — the only file that knows both the flight
 * software and the VM exist.
 *
 * Keeping this separate means flight_states.c and hal_common.c never include
 * a Lua header, and the feature can be compiled out entirely by not linking
 * this object.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_APP_H
#define LUA_APP_H

#include "config.h"
#include "flight_states.h"
#include "lua_check.h"
#include <stdbool.h>

/* Resolve config into platform resources, load the stored script and launch
 * core1. Call once at boot, on core0, before the flight loop. A failure here
 * is reported and never fatal: the flight computer runs without Lua. */
void lua_app_init(const config_t *cfg);

/* Main-loop service: publish flight state, watch core1, nothing blocking. */
void lua_app_service(const flight_context_t *ctx, uint32_t now_ms);

/* Flight events, forwarded to the script's on_event(). */
void lua_app_event(const char *name);

/* Script storage in littlefs. */
#define LUA_SCRIPT_PATH "lua_user.lua"
#define LUA_SCRIPT_MAX 8192

int lua_app_script_read(char *buf, int max);
bool lua_app_script_write(const char *src, int len);

/* Validate a script against the resource set a given configuration would
 * grant -- not against what is currently bound, which is the previous
 * configuration until the next reboot. */
void lua_app_check(const char *src, int len, const config_t *cfg, lua_chk_result_t *out);

/* For the web console. */
int lua_app_console_read(char *buf, int max);

/* Bytes a script has had written to lua_log.txt. Paired with the ring's drop
 * counter on /api/lua/console so "my log has holes" is answerable. */
uint32_t lua_app_log_written(void);

/* One-line status for /api/status. */
const char *lua_app_status(void);

/* True when the board is fully up -- Lua running, or Lua not in play. The
 * startup indication waits for this. */
bool lua_app_ready(void);

#endif
