/*
 * Boot-time configuration of the Lua platform.
 *
 * Kept out of lua_platform.h on purpose. That header is the surface a script
 * can reach and is deliberately free of pins and roles; this one is how the
 * application decides what that surface contains, and nothing in Lua can see
 * it. Configuration runs once, on core0, before core1 exists.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PLATFORM_CFG_H
#define LUA_PLATFORM_CFG_H

#define LUA_NAME_MAX 9 /* matches config.h STR fields: 8 chars + NUL */

typedef enum {
    LUA_ROLE_OFF = 0,
    LUA_ROLE_OUT,   /* digital output                     */
    LUA_ROLE_PWM,   /* dimmable output, software PWM      */
    LUA_ROLE_IN,    /* digital input, pulled down         */
    LUA_ROLE_TX,    /* PIO UART transmit                  */
    LUA_ROLE_RX,    /* PIO UART receive                   */
    LUA_ROLE_PIXEL, /* WS2812/WS2811 string               */
} lua_role_t;

typedef struct {
    lua_role_t role;
    const char *name; /* what Lua calls it; "" when role is OFF */
} lua_pin_cfg_t;

/* Build the resource tables. Returns 0, or -1 if a resource could not be
 * claimed -- which the budget in the board's platform file makes
 * unreachable, so a -1 is a bug rather than an operating condition.
 * Safe to call only before core1 is launched. */
int lua_plat_configure(const lua_pin_cfg_t *cfg, int n, unsigned baud, int pixels);

/* Number of configurable pins this board exposes to Lua. */
int lua_plat_pin_count(void);

/* Called by core1 between VM slices to advance software PWM. Must never
 * block: it runs on the core that has to stay able to answer a park. */
void lua_plat_pin_service(void);

#endif
