/*
 * Config field table — single source of truth for all configuration.
 * Adding a field = one line here. The struct, parser, serializer,
 * defaults, and round-trip test are all generated from this table.
 *
 * X(type, field, key, default_value)
 *
 * Types:
 *   STR   — char[9], truncated to 8 chars  [CFG-07]
 *   U8    — uint8_t
 *   U16   — uint16_t
 *   MODE  — pyro_mode_t (parsed from string: delay/agl/fallen/speed)
 *   UNITS — uint8_t (parsed from string: cm/m/ft)
 *   BOOL  — bool (parsed from string: true/false/1/0)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_FIELDS_H
#define CONFIG_FIELDS_H

/* ── Field table ──────────────────────────────────────────────────── */

/*       type   field          key              default           */
#define CONFIG_FIELDS(X)                                                                                               \
    X(STR, id, "id", "PYRO001")                                                                                        \
    X(STR, name, "name", "My Rocket")                                                                                  \
    X(MODE, pyro1_mode, "pyro1_mode", PYRO_MODE_DELAY)                                                                 \
    X(U16, pyro1_value, "pyro1_value", 0)                                                                              \
    X(MODE, pyro2_mode, "pyro2_mode", PYRO_MODE_AGL)                                                                   \
    X(U16, pyro2_value, "pyro2_value", 300)                                                                            \
    X(UNITS, units, "units", 1)                                                                                        \
    X(U8, beep_mode, "beep_mode", 0)                                                                                   \
    X(U8, max_coast_s, "max_coast_s", 30)                                                                              \
    X(U8, telem_format, "telem_format", 0)                                                                             \
    X(U8, telem_rate_hz, "telem_rate_hz", 10)                                                                          \
    X(U8, log_rate_hz, "log_rate_hz", 50)                                                                              \
    X(BOOL, log_enabled, "log_enabled", true)                                                                          \
    X(BOOL, buzzer_startup, "buzzer_startup", true)                                                                    \
    X(U8, backup_timer, "backup_timer", 30)                                                                            \
    X(U8, landing_timeout, "landing_timeout", 60)                                                                      \
    X(BOOL, lua_enabled, "lua_enabled", false)                                                                         \
    X(U16, lua_baud, "lua_baud", 9600)                                                                                 \
    X(U16, lua_pixels, "lua_pixels", 0)

/* ── Legacy fields: parsed, never written ─────────────────────────
 *
 * pins.ini superseded these. They are still READ, because a board that
 * predates pins.ini has them in its config.ini and pin_store_load() migrates
 * from them when no pins.ini exists -- deleting them outright would move
 * every such board's Lua pins on the next boot.
 *
 * They are no longer SERIALIZED. Eight STR fields cost about 160 bytes, and
 * config.ini serialised to 525 bytes worst case against a 512-byte budget: a
 * legitimately populated config could not be written back at all, and the
 * board answered HTTP 500. Dropping them from the output fixes that, and the
 * keys disappear from a board's config.ini the first time it saves.
 *
 * Nothing new belongs here. */
#define CONFIG_LEGACY_FIELDS(X)                                                                                        \
    X(STR, lua_p18_role, "lua_p18_role", "off")                                                                        \
    X(STR, lua_p18_name, "lua_p18_name", "")                                                                           \
    X(STR, lua_p19_role, "lua_p19_role", "off")                                                                        \
    X(STR, lua_p19_name, "lua_p19_name", "")                                                                           \
    X(STR, lua_p20_role, "lua_p20_role", "off")                                                                        \
    X(STR, lua_p20_name, "lua_p20_name", "")                                                                           \
    X(STR, lua_p21_role, "lua_p21_role", "off")                                                                        \
    X(STR, lua_p21_name, "lua_p21_name", "")

#endif
