/*
 * Boot-time configuration of the Lua platform.
 *
 * Kept out of lua_platform.h, which is the surface a script can reach and is
 * free of pins and roles; this is how the application decides what that
 * surface contains. Configuration runs once, on core0, before the scheduler
 * starts.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LUA_PLATFORM_CFG_H
#define LUA_PLATFORM_CFG_H

#include <stdbool.h>
#include <stdint.h>

#define LUA_NAME_MAX 9 /* matches config.h STR fields: 8 chars + NUL */

typedef enum {
    LUA_ROLE_OFF = 0,
    LUA_ROLE_OUT,    /* digital output                     */
    LUA_ROLE_PWM,    /* dimmable output, software PWM      */
    LUA_ROLE_IN,     /* digital input, pulled down         */
    LUA_ROLE_TX,     /* PIO UART transmit                  */
    LUA_ROLE_RX,     /* PIO UART receive                   */
    LUA_ROLE_PIXEL,  /* WS2812/WS2811 string               */
    LUA_ROLE_BRIDGE, /* one half of a half-bridge pair      */
} lua_role_t;

/* One configured pad: entries carry their own pin, so a released pyro pad,
 * which is not in the board's LUA_PIN_LIST, can be configured too. */
typedef struct {
    uint8_t pin;
    lua_role_t role;
    const char *name; /* what Lua calls it; "" when role is OFF */
} lua_pin_cfg_t;

/* Upper bound on configurable pads: a board's own, plus the three a fully
 * released pyro block frees (two per-channel elements and the common). A
 * number rather than LUA_PIN_COUNT + 3 because the simulator and the host
 * tests share this header and have no board pin table; lua_pio_platform.c
 * asserts the real board fits. */
#define LUA_CFG_MAX 8

/* lua_plat_configure() results. */
#define LUA_PLAT_OK 0
#define LUA_PLAT_CLAIM_FAILED (-1) /* the board's budget makes this a firmware bug */
#define LUA_PLAT_BAD_BAUD (-2)     /* serial is configured and lua_baud is unusable */

/* Build the resource tables. Returns LUA_PLAT_*. Before the scheduler
 * starts. */
int lua_plat_configure(const lua_pin_cfg_t *cfg, int n, uint32_t baud, int pixels);

/* Wire a half-bridge across a released pyro channel. Call after
 * lua_plat_configure(), on core0, at boot. Separate because the bridge pads
 * are not in LUA_PIN_LIST: they become available only when configuration
 * releases the channel.
 *
 * The bridge appears to a script as one output named `name`: 0 drives the
 * midpoint low, anything else drives it high. deadtime_cycles is how long
 * both sides are held off between transitions, in PIO cycles. Returns 0 on
 * success. */
int lua_plat_configure_bridge(uint8_t channel_pin, uint8_t common_pin, const char *name, unsigned deadtime_cycles);

/* Level commands the bridge FIFO could not take, because the Lua task never
 * blocks on it. */
uint32_t lua_plat_bridge_dropped(void);

/* Software PWM: the Lua task advances one phase per tick, so the period is
 * LUA_PWM_STEPS ticks -- 2 s at the 20 ms loop (loop_period.h). A duty of 0
 * and of 100 are steady levels. */
#define LUA_PWM_STEPS 100u

static inline bool lua_pwm_level(uint32_t phase, int duty_percent) {
    return (uint32_t)duty_percent * LUA_PWM_STEPS / 100u > phase % LUA_PWM_STEPS;
}

/* Called by the Lua task once per tick to advance software PWM. Never
 * blocks. */
void lua_plat_pin_service(void);

/* Drive every Lua-owned output to its inactive state and stop anything that
 * would keep driving one. Called from the flight task after the Lua task has
 * been stopped, which is why it is here and not in lua_platform.h: nothing a
 * script can call should be able to do this.
 *
 * Stopping the task puts nothing down. A pin left high stays high, and a PIO
 * state machine keeps clocking its pad after the task that fed it is gone.
 *
 * Safe to call more than once, and when Lua was never started. */
void lua_plat_safe_outputs(void);

#endif
