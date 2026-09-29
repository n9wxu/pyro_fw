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
#include <stdint.h>

/* Resolve config into platform resources, load the stored script and launch
 * core1. Call once at boot, on core0, before the flight loop. A failure here
 * is reported and never fatal: the flight computer runs without Lua. */
void lua_app_init(const config_t *cfg);

/* The flight task's service: publish flight state, watch the Lua task, and
 * drain the script's log ring into the flight log. Touches no file and never
 * blocks.
 *
 * A script's log() output is appended to the flight log as event rows, in
 * the same ring as the samples, which the storage task writes. */
void lua_app_service(const flight_context_t *ctx, uint32_t now_ms);

/* Asks the Lua task for this period's tick. Once a period, after the flight
 * work; never blocks. */
void lua_app_dispatch(void);

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

/* Bytes of script output the flight log took. Paired on /api/lua/console with
 * the ring's drop counter and the log buffer's, so "my log has holes" is
 * answerable -- and with the reminder that there is no log at all until
 * launch, because on the ground the console IS the log. */
uint32_t lua_app_log_written(void);

/* One-line status for /api/status. */
const char *lua_app_status(void);

/* True when the board is fully up -- Lua running, or Lua not in play. The
 * startup indication waits for this. */
bool lua_app_ready(void);

#endif
