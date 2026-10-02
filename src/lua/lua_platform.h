/*
 * What the platform provides to Lua.
 *
 * This is the whole surface a script can reach. It contains no pin numbers,
 * no peripheral instances and no SDK types (invariant L5 of
 * thoughts/shared/plans/2026-09-21-lua-user-programs-core1.md): resources
 * are reached by name, so a script cannot express access to something
 * configuration did not grant it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PLATFORM_H
#define LUA_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "lua_iface.h"

/* Outputs, inputs, serial ports and LED strings are entries in the interface
 * table (lua_iface.h): a board adds a feature by publishing a vtable, not by
 * growing this header. */

/* ── Read-only flight state ───────────────────────────────────────── */

int32_t lua_plat_pressure_pa(void);
int32_t lua_plat_altitude_cm(void);
int32_t lua_plat_speed_cms(void);
int32_t lua_plat_max_altitude_cm(void);
int lua_plat_flight_state(void);
uint32_t lua_plat_time_ms(void);

/* The rest of what the built-in telemetry formatter emits, so a script can
 * produce the same sentences. */
int lua_plat_under_thrust(void);
int lua_plat_apogee_detected(void);
uint32_t lua_plat_telem_seq(void);

/* Pyro status is read-only by construction (invariant L11): there is no
 * setter anywhere in this header, so no binding can be written against one. */
#define LUA_PYRO_CONTINUITY (1u << 0)
#define LUA_PYRO_FIRED (1u << 1)
#define LUA_PYRO_FAULT (1u << 2)
#define LUA_PYRO_ARMED (1u << 3)
int lua_plat_pyro_status(int channel); /* channel 1 or 2 */

/* Raw continuity counts: the booleans above round a degraded connector to
 * "good". What a count is worth differs by board. MK1B's AP2192 has an
 * internal ~100 ohm output bleed which, against the 100k pull-up, holds the
 * node near 4 counts with the high side off, so there the count reports the
 * driven state rather than load presence. */
int lua_plat_pyro_adc(int channel);

/* True when configuration has released this channel to Lua. The continuity
 * and armed flags describe a pyro channel, so they stop meaning anything once
 * one is released; the raw count keeps being sampled either way. */
int lua_plat_pyro_released(int channel);

/* ── Host clock ───────────────────────────────────────────────────
 *
 * Microseconds, free-running, wrap-safe when compared with a signed delta.
 * Not a binding: it is how the VM host time-boxes a work unit, here so
 * pyro_lua.c stays free of SDK headers. */
uint32_t lua_plat_now_us(void);

/* ── Console ──────────────────────────────────────────────────────── */

/* Where print() goes: the simulator's terminal pane, the target's web
 * console. */
void lua_plat_console_out(const char *s, int len);

/* Where log() goes: handed to the application, which owns the file -- a
 * script never writes flash (invariant L6). Never blocks; drops if the
 * application is not draining, and counts the drop. */
void lua_plat_log_write(const char *s, int len);

#endif
