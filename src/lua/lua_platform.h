/*
 * What the platform provides to Lua.
 *
 * This is the whole surface a script can reach. It deliberately contains no
 * pin numbers, no peripheral instances and no SDK types (invariant L5): the
 * platform resolves configuration into a numbered list of named resources,
 * and Lua addresses them by name. A script therefore cannot express access to
 * something configuration did not grant it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PLATFORM_H
#define LUA_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "lua_iface.h"

/* ── Resources ────────────────────────────────────────────────────
 *
 * Outputs, inputs, serial ports and LED strings are not four APIs here. They
 * are entries in the interface table, published by whoever configured the
 * hardware and reached by name and kind. See lua_iface.h; a board adds a
 * feature by publishing a vtable, not by growing this header. */

/* ── Read-only flight state ───────────────────────────────────────── */

int32_t lua_plat_pressure_pa(void);
int32_t lua_plat_altitude_cm(void);
int32_t lua_plat_speed_cms(void);
int32_t lua_plat_max_altitude_cm(void);
int lua_plat_flight_state(void);
uint32_t lua_plat_time_ms(void);

/* The rest of what the built-in telemetry formatter emits, so a script can
 * produce the same sentences rather than a subset of them. */
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

/* Raw continuity counts. The booleans above round a degraded connector to
 * "good"; only the count shows it, which is why telemetry carries it.
 *
 * What a count is worth differs by board and the API does not pretend
 * otherwise. MK1A's sense node swings nearly full scale. MK1B's AP2192 has an
 * internal ~100 ohm output bleed which, against the 100k pull-up, holds the
 * node near 4 counts with the high side off -- so there the count reports the
 * driven state rather than load presence. Raw either way: a boolean would
 * round MK1B's four counts into a confident lie. */
int lua_plat_pyro_adc(int channel);

/* True when configuration has released this channel to Lua. The continuity
 * and armed flags describe a pyro channel, so they stop meaning anything once
 * one is released; the raw count keeps being sampled either way. */
int lua_plat_pyro_released(int channel);

/* ── Host clock ───────────────────────────────────────────────────
 *
 * Microseconds, free-running, wrap-safe when compared with a signed delta.
 * Not a binding -- nothing in the Lua API exposes it. It is how the VM host
 * time-boxes a work unit, and it lives here so pyro_lua.c stays free of SDK
 * headers and the host tests can supply their own clock. */
uint32_t lua_plat_now_us(void);

/* ── Console ──────────────────────────────────────────────────────── */

/* Where print() goes. On the simulator this is the terminal pane; on the
 * target it is the WebSocket console. */
void lua_plat_console_out(const char *s, int len);

/* Where log() goes: handed to the application, which owns the file. A script
 * cannot write flash itself -- on the target it runs on core1, and core1
 * touching flash is the hazard the whole design exists to prevent. Never
 * blocks; drops if the application is not draining, and the drop is counted
 * rather than hidden. */
void lua_plat_log_write(const char *s, int len);

#endif
