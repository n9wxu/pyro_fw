/*
 * What the platform provides to Lua.
 *
 * This is the whole surface a script can reach. It deliberately contains no
 * pin numbers, no peripheral instances and no SDK types (invariant L5): the
 * platform resolves configuration into a numbered list of named resources,
 * and Lua addresses them by name. A script therefore cannot express access to
 * something configuration did not grant it.
 *
 * Two implementations:
 *   boards/sim/lua_platform_sim.c    simulated pins and a UART buffer
 *   boards/mk1c/lua_platform_mk1c.c  real GPIO and uart1/i2c0   (not yet)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PLATFORM_H
#define LUA_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

/* ── Outputs ──────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    bool dimmable; /* true if the underlying pin can do PWM */
} lua_output_desc_t;

int lua_plat_output_count(void);
const lua_output_desc_t *lua_plat_output_desc(int idx);
void lua_plat_output_set(int idx, int value); /* 0-100; 0/100 for digital */
int lua_plat_output_get(int idx);

/* ── Inputs ───────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
} lua_input_desc_t;

int lua_plat_input_count(void);
const lua_input_desc_t *lua_plat_input_desc(int idx);
int lua_plat_input_get(int idx);

/* ── Serial ───────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
} lua_serial_desc_t;

int lua_plat_serial_count(void);
const lua_serial_desc_t *lua_plat_serial_desc(int idx);
int lua_plat_serial_write(int idx, const char *s, int len);
int lua_plat_serial_read(int idx, char *buf, int max);

/* ── Read-only flight state ───────────────────────────────────────── */

int32_t lua_plat_pressure_pa(void);
int32_t lua_plat_altitude_cm(void);
int32_t lua_plat_speed_cms(void);
int32_t lua_plat_max_altitude_cm(void);
int lua_plat_flight_state(void);
uint32_t lua_plat_time_ms(void);

/* Pyro status is read-only by construction (invariant L11): there is no
 * setter anywhere in this header, so no binding can be written against one. */
#define LUA_PYRO_CONTINUITY (1u << 0)
#define LUA_PYRO_FIRED (1u << 1)
#define LUA_PYRO_FAULT (1u << 2)
#define LUA_PYRO_ARMED (1u << 3)
int lua_plat_pyro_status(int channel); /* channel 1 or 2 */

/* ── Console ──────────────────────────────────────────────────────── */

/* Where print() goes. On the simulator this is the terminal pane; on the
 * target it is the WebSocket console. */
void lua_plat_console_out(const char *s, int len);

#endif
