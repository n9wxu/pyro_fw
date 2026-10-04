/*
 * Configuration: the struct, parser and serializer generated from
 * config_fields.h [CFG-TABLE-02, SYS-CFG-04].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PYRO_MODE_NONE = 0,
    PYRO_MODE_FALLEN = 1,
    PYRO_MODE_AGL = 2,
    PYRO_MODE_SPEED = 3,
    PYRO_MODE_DELAY = 4
} pyro_mode_t;

/* [CFG-03] The unit pyro*_value and the altitude beep-out are in. */
typedef enum {
    UNITS_CM = 0,
    UNITS_M = 1,
    UNITS_FT = 2,
} config_units_t;

/* The flight log's plan [FLT-LOG-07, DD-064]; log_plan.h says what each keeps. */
typedef enum {
    LOG_RATE_1HZ = 0,
    LOG_RATE_EVENTS = 1,
    LOG_RATE_FULL = 2,
} log_rate_t;

/* The longest config.ini the board reads back, and so the longest it writes. */
#define CONFIG_INI_MAX 1024

#include "config_fields.h"

#define X_FIELD_STR(field) char field[9];
#define X_FIELD_U8(field) uint8_t field;
#define X_FIELD_U16(field) uint16_t field;
#define X_FIELD_U32(field) uint32_t field;
#define X_FIELD_MODE(field) uint8_t field;
#define X_FIELD_UNITS(field) uint8_t field;
#define X_FIELD_BOOL(field) bool field;
#define X_FIELD_LOGRATE(field) uint8_t field;
#define X_STRUCT(type, field, key, def, min, max) X_FIELD_##type(field)

typedef struct {
    CONFIG_FIELDS(X_STRUCT)
} config_t;

#undef X_STRUCT
#undef X_FIELD_STR
#undef X_FIELD_U8
#undef X_FIELD_U16
#undef X_FIELD_U32
#undef X_FIELD_MODE
#undef X_FIELD_UNITS
#undef X_FIELD_BOOL
#undef X_FIELD_LOGRATE

void config_set_defaults(config_t *cfg);

/* Parse INI text over cfg [CFG-02, CFG-06..09]: a key the text does not name
 * keeps its value. A value that does not parse, or is outside its row's
 * [min, max], is rejected and the field keeps its value [SYS-CFG-03]; an
 * unnamed pyro mode is stored as none [CFG-04]. Returns how many values were
 * rejected. Mutates buf. */
int config_parse_ini(char *buf, config_t *cfg);

/* Bytes written (excluding NUL), or -1 when it does not fit max_len. */
int config_serialize_ini(const config_t *cfg, char *buf, int max_len);

/* The default config as INI text (static, do not free). */
const char *config_default_ini(void);

/* What hal_config_load() makes of config.ini [CFG-05, FLT-BOOT-18]. */
typedef enum {
    CONFIG_FILE_LOADED,     /* read whole and parsed over the defaults */
    CONFIG_FILE_MISSING,    /* no file: the defaults, to be written for next time */
    CONFIG_FILE_UNREADABLE, /* the defaults, and the file left as it is */
} config_file_t;

/* n is hal_fs_read_file("config.ini", buf, buf_size - 1)'s answer. cfg gets
 * the defaults, and the file's values over them when it was read whole: a
 * read that filled the buffer may have been cut short, and a cut line
 * parses as a wrong value. rejected, when not NULL, receives
 * config_parse_ini()'s count. */
config_file_t config_from_file(config_t *cfg, char *buf, int buf_size, int n, int *rejected);

/* The config.ini spelling of a pyro mode: none/delay/agl/fallen/speed. The one
 * table every writer of a mode name uses, so a log header cannot disagree
 * with the file that configured it. */
const char *config_mode_name(uint8_t mode);

/* The config.ini spelling of a log rate: 1hz/events/full. */
const char *config_log_rate_name(uint8_t rate);

/* The config.ini spelling of a unit: cm/m/ft. */
const char *config_units_name(uint8_t units);

#endif
