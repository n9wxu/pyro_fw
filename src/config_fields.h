/*
 * The configuration's one table [SYS-CFG-04]: the struct, the
 * parser, the serializer, the defaults and the round-trip test all come from
 * it.
 *
 * X(type, field, key, default, min, max)
 *
 *   STR     char[9], truncated to 8 characters [CFG-07]; min, max unused
 *   U8, U16, U32  an integer in [min, max] [SYS-CFG-03]
 *   MODE    pyro_mode_t: none/delay/agl/fallen/speed [CFG-04]
 *   UNITS   config_units_t: cm/m/ft [CFG-03]
 *   BOOL    true/false/1/0
 *   LOGRATE log_rate_t: 1hz/events/full [FLT-LOG-07]
 *
 * pyro*_value is in the configured units, so the cm ceiling is the field's.
 * lua_baud's floor keeps the PIO UART's divider, sys_clk / (8 * baud)
 * (lua_pio.pio), inside its 16-bit integer part (RP2040 datasheet §3.5.5):
 * 52083 at 125 MHz.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_FIELDS_H
#define CONFIG_FIELDS_H

/*       type     field            key                default           min  max */
#define CONFIG_FIELDS(X)                                                                                               \
    X(STR, id, "id", "PYRO001", 0, 0)                                                                                  \
    X(STR, name, "name", "MyRocket", 0, 0)                                                                             \
    X(MODE, pyro1_mode, "pyro1_mode", PYRO_MODE_DELAY, 0, 0)                                                           \
    X(U16, pyro1_value, "pyro1_value", 0, 0, 65535)                                                                    \
    X(MODE, pyro2_mode, "pyro2_mode", PYRO_MODE_AGL, 0, 0)                                                             \
    X(U16, pyro2_value, "pyro2_value", 300, 0, 65535)                                                                  \
    X(UNITS, units, "units", UNITS_M, 0, 0)                                                                            \
    X(U16, pyro1_refire_speed, "pyro1_refire_speed", 0, 0, 65535)                                                      \
    X(U16, pyro2_refire_speed, "pyro2_refire_speed", 0, 0, 65535)                                                      \
    X(U16, emergency_fire_speed, "emergency_fire_speed", 0, 0, 65535)                                                  \
    X(U16, refire_interval, "refire_interval", 0, 0, 65535)                                                            \
    X(U16, fire_gap, "fire_gap", 0, 0, 65535)                                                                          \
    X(STR, estimator, "estimator", "lumped", 0, 0)                                                                     \
    X(LOGRATE, log_rate, "log_rate", LOG_RATE_1HZ, 0, 0)                                                               \
    X(U8, landing_timeout, "landing_timeout", 60, 0, 255)                                                              \
    X(BOOL, lua_enabled, "lua_enabled", false, 0, 0)                                                                   \
    X(U32, lua_baud, "lua_baud", 9600, 300, 921600)                                                                    \
    X(U16, lua_pixels, "lua_pixels", 0, 0, 65535)

#endif
