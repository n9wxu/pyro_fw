/*
 * Configuration module — X-macro generated struct, parser, serializer.
 * [CFG-TABLE-01, CFG-TABLE-02, SYS-CFG-04]
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ── Pyro mode enum (needed by config_fields.h defaults) ──────────── */

typedef enum {
    PYRO_MODE_NONE = 0,
    PYRO_MODE_FALLEN = 1,
    PYRO_MODE_AGL = 2,
    PYRO_MODE_SPEED = 3,
    PYRO_MODE_DELAY = 4
} pyro_mode_t;

/* ── Config struct — generated from X-macro table ─────────────────── */

#include "config_fields.h"

#define X_FIELD_STR(type, field, key, def) char field[9];
#define X_FIELD_U8(type, field, key, def) uint8_t field;
#define X_FIELD_U16(type, field, key, def) uint16_t field;
#define X_FIELD_MODE(type, field, key, def) uint8_t field;
#define X_FIELD_UNITS(type, field, key, def) uint8_t field;
#define X_FIELD_BOOL(type, field, key, def) bool field;

/* Dispatch: X_FIELD_##type expands to the correct declaration */
#define X_STRUCT(type, field, key, def) X_FIELD_##type(type, field, key, def)

typedef struct {
    CONFIG_FIELDS(X_STRUCT)
} config_t;

#undef X_STRUCT
#undef X_FIELD_STR
#undef X_FIELD_U8
#undef X_FIELD_U16
#undef X_FIELD_MODE
#undef X_FIELD_UNITS
#undef X_FIELD_BOOL

/* ── API ──────────────────────────────────────────────────────────── */

/* Set all fields to compile-time defaults */
void config_set_defaults(config_t *cfg);

/* Parse INI text into cfg (preserves unset fields) [CFG-02, CFG-06..09] */
void config_parse_ini(char *buf, config_t *cfg);

/* Serialize cfg to INI text. Returns bytes written (excluding NUL).
 * buf must be at least max_len bytes. */
int config_serialize_ini(const config_t *cfg, char *buf, int max_len);

/* Return the default config as an INI string (static, do not free). */
const char *config_default_ini(void);

#endif
