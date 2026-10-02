/*
 * Configuration: parser, serializer and defaults, generated from
 * config_fields.h [CFG-TABLE-01, CFG-02, CFG-06..09].
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"
#include "hal.h" /* HAL_FS_NOENT */
#include "ini_tokenizer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* ── Names ─────────────────────────────────────────────────────────── */

static const struct {
    const char *name;
    uint8_t mode;
} MODE_NAMES[] = {
    {"none", PYRO_MODE_NONE},     {"delay", PYRO_MODE_DELAY}, {"agl", PYRO_MODE_AGL},
    {"fallen", PYRO_MODE_FALLEN}, {"speed", PYRO_MODE_SPEED},
};

/* Anything unrecognised is written as "none": a mode this board cannot name
 * must come back from config.ini as one that never fires [CFG-04]. */
const char *config_mode_name(uint8_t m) {
    for (unsigned i = 0; i < COUNT_OF(MODE_NAMES); i++)
        if (MODE_NAMES[i].mode == m)
            return MODE_NAMES[i].name;
    return "none";
}

static const char *const UNITS_NAMES[] = {[UNITS_CM] = "cm", [UNITS_M] = "m", [UNITS_FT] = "ft"};

const char *config_units_name(uint8_t u) {
    return u < COUNT_OF(UNITS_NAMES) ? UNITS_NAMES[u] : UNITS_NAMES[UNITS_M];
}

static const char *const LOG_RATE_NAMES[] = {
    [LOG_RATE_1HZ] = "1hz", [LOG_RATE_EVENTS] = "events", [LOG_RATE_FULL] = "full"};

const char *config_log_rate_name(uint8_t rate) {
    return rate < COUNT_OF(LOG_RATE_NAMES) ? LOG_RATE_NAMES[rate] : LOG_RATE_NAMES[LOG_RATE_1HZ];
}

/* ── Value parsers: false, and *out untouched, for a value that does not
 *    parse or is out of range ────────────────────────────────────────── */

static bool parse_name(const char *s, const char *const *names, unsigned n, uint8_t *out) {
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = (uint8_t)i;
            return true;
        }
    }
    return false;
}

/* [CFG-04] Unnamed is none, and still reported. */
static bool parse_mode(const char *s, uint8_t *out) {
    for (unsigned i = 0; i < COUNT_OF(MODE_NAMES); i++) {
        if (strcmp(s, MODE_NAMES[i].name) == 0) {
            *out = MODE_NAMES[i].mode;
            return true;
        }
    }
    *out = PYRO_MODE_NONE;
    return false;
}

static bool parse_bool(const char *s, bool *out) {
    if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* Decimal digits only: no sign, no blank, nothing after. */
static bool parse_uint(const char *s, uint32_t min, uint32_t max, uint32_t *out) {
    if (s[0] < '0' || s[0] > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < min || v > max)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* ── Defaults ─────────────────────────────────────────────────────── */

/* A shipped default must be one the parser would accept. */
#define X_CHECK_STR(field, key, def, min, max)                                                                         \
    _Static_assert(sizeof(def) <= sizeof(((config_t *)0)->field), "default for " key " does not fit its field");
#define X_CHECK_INT(field, key, def, min, max)                                                                         \
    _Static_assert((def) >= (min) && (def) <= (max), "default for " key " is out of its range");                       \
    _Static_assert((uint64_t)(max) <= (uint64_t)(__typeof__(((config_t *)0)->field))-1,                                \
                   key "'s max overflows its field");
#define X_CHECK_U8 X_CHECK_INT
#define X_CHECK_U16 X_CHECK_INT
#define X_CHECK_U32 X_CHECK_INT
#define X_CHECK_MODE(field, key, def, min, max)
#define X_CHECK_UNITS(field, key, def, min, max)
#define X_CHECK_BOOL(field, key, def, min, max)
#define X_CHECK_LOGRATE(field, key, def, min, max)
#define X_CHECK(type, field, key, def, min, max) X_CHECK_##type(field, key, def, min, max)
CONFIG_FIELDS(X_CHECK)
#undef X_CHECK

void config_set_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
#define X_DEF_STR(field, def)                                                                                          \
    strncpy(cfg->field, def, sizeof(cfg->field) - 1);                                                                  \
    cfg->field[sizeof(cfg->field) - 1] = '\0';
#define X_DEF_VALUE(field, def) cfg->field = (def);
#define X_DEF_U8 X_DEF_VALUE
#define X_DEF_U16 X_DEF_VALUE
#define X_DEF_U32 X_DEF_VALUE
#define X_DEF_MODE X_DEF_VALUE
#define X_DEF_UNITS X_DEF_VALUE
#define X_DEF_BOOL X_DEF_VALUE
#define X_DEF_LOGRATE X_DEF_VALUE
#define X_DEFAULTS(type, field, key, def, min, max) X_DEF_##type(field, def)
    CONFIG_FIELDS(X_DEFAULTS)
#undef X_DEFAULTS
}

/* ── Parser ───────────────────────────────────────────────────────── */

typedef struct {
    config_t *cfg;
    int rejected;
} parse_ctx_t;

/* One key=value. Unknown keys are ignored [CFG-08]. */
static void config_parse_pair(const char *key, const char *val, void *arg) {
    parse_ctx_t *pc = arg;
    config_t *cfg = pc->cfg;
    bool ok = true;
    uint32_t u;
    uint8_t b;
#define X_PARSE_STR(field)                                                                                             \
    strncpy(cfg->field, val, sizeof(cfg->field) - 1);                                                                  \
    cfg->field[sizeof(cfg->field) - 1] = '\0';
#define X_PARSE_INT(field, min, max)                                                                                   \
    if ((ok = parse_uint(val, (min), (max), &u)))                                                                      \
        cfg->field = (__typeof__(cfg->field))u;
#define X_PARSE_MODE(field) ok = parse_mode(val, &cfg->field);
#define X_PARSE_UNITS(field)                                                                                           \
    if ((ok = parse_name(val, UNITS_NAMES, COUNT_OF(UNITS_NAMES), &b)))                                                \
        cfg->field = b;
#define X_PARSE_BOOL(field) ok = parse_bool(val, &cfg->field);
#define X_PARSE_LOGRATE(field)                                                                                         \
    if ((ok = parse_name(val, LOG_RATE_NAMES, COUNT_OF(LOG_RATE_NAMES), &b)))                                          \
        cfg->field = b;
#define X_PARSE_ONE_STR(field, min, max) X_PARSE_STR(field)
#define X_PARSE_ONE_U8(field, min, max) X_PARSE_INT(field, min, max)
#define X_PARSE_ONE_U16(field, min, max) X_PARSE_INT(field, min, max)
#define X_PARSE_ONE_U32(field, min, max) X_PARSE_INT(field, min, max)
#define X_PARSE_ONE_MODE(field, min, max) X_PARSE_MODE(field)
#define X_PARSE_ONE_UNITS(field, min, max) X_PARSE_UNITS(field)
#define X_PARSE_ONE_BOOL(field, min, max) X_PARSE_BOOL(field)
#define X_PARSE_ONE_LOGRATE(field, min, max) X_PARSE_LOGRATE(field)
#define X_PARSE(type, field, key_str, def, min, max)                                                                   \
    if (strcmp(key, key_str) == 0) {                                                                                   \
        X_PARSE_ONE_##type(field, min, max) goto done;                                                                 \
    }
    CONFIG_FIELDS(X_PARSE)
#undef X_PARSE
    return;
done:
    if (!ok)
        pc->rejected++;
}

/* [CFG-02, CFG-06..10, SYS-CFG-03] */
int config_parse_ini(char *buf, config_t *cfg) {
    parse_ctx_t pc = {cfg, 0};
    ini_for_each(buf, config_parse_pair, &pc);
    return pc.rejected;
}

config_file_t config_from_file(config_t *cfg, char *buf, int buf_size, int n, int *rejected) {
    config_set_defaults(cfg);
    if (rejected)
        *rejected = 0;
    if (n == HAL_FS_NOENT)
        return CONFIG_FILE_MISSING;
    if (n < 0 || n >= buf_size - 1)
        return CONFIG_FILE_UNREADABLE;
    buf[n] = '\0';
    int r = config_parse_ini(buf, cfg);
    if (rejected)
        *rejected = r;
    return CONFIG_FILE_LOADED;
}

/* ── Serializer ───────────────────────────────────────────────────── */

int config_serialize_ini(const config_t *cfg, char *buf, int max_len) {
    int pos = 0;

/* snprintf returns what it WOULD have written, so an unguarded `pos += n`
 * hands the next call a negative size, which converts to a huge size_t. */
#define APPEND(fmt, ...)                                                                                               \
    do {                                                                                                               \
        if (pos >= max_len)                                                                                            \
            return -1;                                                                                                 \
        int n = snprintf(buf + pos, (size_t)(max_len - pos), fmt, ##__VA_ARGS__);                                      \
        if (n < 0 || n >= max_len - pos)                                                                               \
            return -1;                                                                                                 \
        pos += n;                                                                                                      \
    } while (0)

    APPEND("[pyro]\r\n");

#define X_SER_STR(key, v) APPEND("%s=%s\r\n", key, v);
#define X_SER_U8(key, v) APPEND("%s=%lu\r\n", key, (unsigned long)(v));
#define X_SER_U16 X_SER_U8
#define X_SER_U32 X_SER_U8
#define X_SER_MODE(key, v) APPEND("%s=%s\r\n", key, config_mode_name(v));
#define X_SER_UNITS(key, v) APPEND("%s=%s\r\n", key, config_units_name(v));
#define X_SER_BOOL(key, v) APPEND("%s=%s\r\n", key, (v) ? "true" : "false");
#define X_SER_LOGRATE(key, v) APPEND("%s=%s\r\n", key, config_log_rate_name(v));
#define X_SERIALIZE(type, field, key, def, min, max) X_SER_##type(key, cfg->field)
    CONFIG_FIELDS(X_SERIALIZE)
#undef X_SERIALIZE
#undef APPEND

    return pos;
}

/* ── Default INI string ───────────────────────────────────────────── */

const char *config_default_ini(void) {
    static char ini[CONFIG_INI_MAX];
    static bool done = false;
    if (!done) {
        config_t defaults;
        config_set_defaults(&defaults);
        config_serialize_ini(&defaults, ini, sizeof(ini));
        done = true;
    }
    return ini;
}
