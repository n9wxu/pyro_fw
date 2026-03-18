/*
 * Configuration module — X-macro generated parser, serializer, defaults.
 * [CFG-TABLE-01, CFG-02, CFG-06..09]
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Mode/units string helpers ────────────────────────────────────── */

static uint8_t parse_mode(const char *s) {
    if (strcmp(s, "delay") == 0)
        return PYRO_MODE_DELAY;
    if (strcmp(s, "agl") == 0)
        return PYRO_MODE_AGL;
    if (strcmp(s, "fallen") == 0)
        return PYRO_MODE_FALLEN;
    if (strcmp(s, "speed") == 0)
        return PYRO_MODE_SPEED;
    return 0;
}

static const char *mode_to_str(uint8_t m) {
    switch (m) {
    case PYRO_MODE_DELAY:
        return "delay";
    case PYRO_MODE_AGL:
        return "agl";
    case PYRO_MODE_FALLEN:
        return "fallen";
    case PYRO_MODE_SPEED:
        return "speed";
    default:
        return "delay";
    }
}

static uint8_t parse_units(const char *s) {
    if (strcmp(s, "cm") == 0)
        return 0;
    if (strcmp(s, "m") == 0)
        return 1;
    if (strcmp(s, "ft") == 0)
        return 2;
    return 1; /* default: meters */
}

static const char *units_to_str(uint8_t u) {
    switch (u) {
    case 0:
        return "cm";
    case 1:
        return "m";
    case 2:
        return "ft";
    default:
        return "m";
    }
}

/* ── Defaults ─────────────────────────────────────────────────────── */

void config_set_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

#define X_DEF_STR(type, field, key, def)                                                                               \
    strncpy(cfg->field, def, 8);                                                                                       \
    cfg->field[8] = '\0';
#define X_DEF_U8(type, field, key, def) cfg->field = (uint8_t)(def);
#define X_DEF_U16(type, field, key, def) cfg->field = (uint16_t)(def);
#define X_DEF_MODE(type, field, key, def) cfg->field = (uint8_t)(def);
#define X_DEF_UNITS(type, field, key, def) cfg->field = (uint8_t)(def);
#define X_DEF_BOOL(type, field, key, def) cfg->field = (def);

#define X_DEFAULTS(type, field, key, def) X_DEF_##type(type, field, key, def)
    CONFIG_FIELDS(X_DEFAULTS)
#undef X_DEFAULTS
#undef X_DEF_STR
#undef X_DEF_U8
#undef X_DEF_U16
#undef X_DEF_MODE
#undef X_DEF_UNITS
#undef X_DEF_BOOL
}

/* ── Parser ───────────────────────────────────────────────────────── */

/* Parse a single key=value pair into the config struct.
 * Unknown keys are silently ignored [CFG-08]. */
static void config_parse_line(const char *key, const char *val, config_t *cfg) {
#define X_PARSE_STR(type, field, key_str, def)                                                                         \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        strncpy(cfg->field, val, 8);                                                                                   \
        cfg->field[8] = '\0';                                                                                          \
        return;                                                                                                        \
    }
#define X_PARSE_U8(type, field, key_str, def)                                                                          \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        cfg->field = (uint8_t)atoi(val);                                                                               \
        return;                                                                                                        \
    }
#define X_PARSE_U16(type, field, key_str, def)                                                                         \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        cfg->field = (uint16_t)atoi(val);                                                                              \
        return;                                                                                                        \
    }
#define X_PARSE_MODE(type, field, key_str, def)                                                                        \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        cfg->field = parse_mode(val);                                                                                  \
        return;                                                                                                        \
    }
#define X_PARSE_UNITS(type, field, key_str, def)                                                                       \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        cfg->field = parse_units(val);                                                                                 \
        return;                                                                                                        \
    }
#define X_PARSE_BOOL(type, field, key_str, def)                                                                        \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        cfg->field = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);                                              \
        return;                                                                                                        \
    }

#define X_PARSE(type, field, key_str, def) X_PARSE_##type(type, field, key_str, def)
    CONFIG_FIELDS(X_PARSE)
#undef X_PARSE
#undef X_PARSE_STR
#undef X_PARSE_U8
#undef X_PARSE_U16
#undef X_PARSE_MODE
#undef X_PARSE_UNITS
#undef X_PARSE_BOOL
    /* Unknown key — silently ignored [CFG-08] */
}

/* [CFG-02, CFG-06..09] Parse INI text. Preserves fields not in buf. */
void config_parse_ini(char *buf, config_t *cfg) {
    char *line = buf;
    while (*line) {
        /* Skip leading whitespace */
        while (*line == ' ' || *line == '\t')
            line++;
        /* Find end of line */
        char *nl = line;
        while (*nl && *nl != '\r' && *nl != '\n')
            nl++;
        char saved = *nl;
        *nl = '\0';
        /* Parse key=value (skip section headers, comments, blank lines) */
        char *eq = strchr(line, '=');
        if (eq && line[0] != '[' && line[0] != ';' && line[0] != '#') {
            *eq = '\0';
            config_parse_line(line, eq + 1, cfg);
            *eq = '='; /* restore for re-parsing if needed */
        }
        *nl = saved;
        /* Advance past newline(s) */
        if (*nl == '\r')
            nl++;
        if (*nl == '\n')
            nl++;
        line = nl;
    }
}

/* ── Serializer ───────────────────────────────────────────────────── */

int config_serialize_ini(const config_t *cfg, char *buf, int max_len) {
    int pos = 0;

#define APPEND(fmt, ...)                                                                                               \
    do {                                                                                                               \
        int n = snprintf(buf + pos, max_len - pos, fmt, ##__VA_ARGS__);                                                \
        if (n > 0)                                                                                                     \
            pos += n;                                                                                                  \
    } while (0)

    APPEND("[pyro]\r\n");

#define X_SER_STR(type, field, key, def) APPEND("%s=%s\r\n", key, cfg->field);
#define X_SER_U8(type, field, key, def) APPEND("%s=%u\r\n", key, (unsigned)cfg->field);
#define X_SER_U16(type, field, key, def) APPEND("%s=%u\r\n", key, (unsigned)cfg->field);
#define X_SER_MODE(type, field, key, def) APPEND("%s=%s\r\n", key, mode_to_str(cfg->field));
#define X_SER_UNITS(type, field, key, def) APPEND("%s=%s\r\n", key, units_to_str(cfg->field));
#define X_SER_BOOL(type, field, key, def) APPEND("%s=%s\r\n", key, cfg->field ? "true" : "false");

#define X_SERIALIZE(type, field, key, def) X_SER_##type(type, field, key, def)
    CONFIG_FIELDS(X_SERIALIZE)
#undef X_SERIALIZE
#undef X_SER_STR
#undef X_SER_U8
#undef X_SER_U16
#undef X_SER_MODE
#undef X_SER_UNITS
#undef X_SER_BOOL
#undef APPEND

    return pos;
}

/* ── Default INI string ───────────────────────────────────────────── */

const char *config_default_ini(void) {
    static char ini[512];
    static bool done = false;
    if (!done) {
        config_t defaults;
        config_set_defaults(&defaults);
        config_serialize_ini(&defaults, ini, sizeof(ini));
        done = true;
    }
    return ini;
}
