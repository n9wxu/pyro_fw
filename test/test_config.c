/*
 * Config module tests — round-trip, defaults, parser, serializer.
 * [CFG-TABLE-02] Every field survives serialize → parse.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [CFG-01..09, SYS-CFG-04].
 */
#include "unity.h"
#include "config.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Defaults ─────────────────────────────────────────────────────── */

void test_config_defaults(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    TEST_ASSERT_EQUAL_STRING("PYRO001", cfg.id);
    TEST_ASSERT_EQUAL_STRING("MyRocket", cfg.name);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(0, cfg.pyro1_value);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
    TEST_ASSERT_EQUAL(1, cfg.units); /* meters */
    /* [PYR-REFIRE-01, FLT-EMRG-01] A speed of zero disables its rule; an
     * interval or a gap of zero takes the board's default [PYR-BOARD-01]. */
    TEST_ASSERT_EQUAL(0, cfg.pyro1_refire_speed);
    TEST_ASSERT_EQUAL(0, cfg.pyro2_refire_speed);
    TEST_ASSERT_EQUAL(0, cfg.emergency_fire_speed);
    TEST_ASSERT_EQUAL(0, cfg.refire_interval);
    TEST_ASSERT_EQUAL(0, cfg.fire_gap);
    TEST_ASSERT_EQUAL_MESSAGE(LOG_RATE_1HZ, cfg.log_rate, "one row a second unless asked [DD-064]");
    TEST_ASSERT_FALSE(cfg.lua_enabled);
}

/* ── Round-trip: serialize → parse → compare [CFG-TABLE-02] ───────── */

void test_config_roundtrip_defaults(void) {
    config_t original, restored;
    config_set_defaults(&original);

    char buf[512];
    int len = config_serialize_ini(&original, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);

    /* Parse into a zeroed struct — all fields should be restored */
    memset(&restored, 0, sizeof(restored));
    config_parse_ini(buf, &restored);

    TEST_ASSERT_EQUAL_STRING(original.id, restored.id);
    TEST_ASSERT_EQUAL_STRING(original.name, restored.name);
    TEST_ASSERT_EQUAL(original.pyro1_mode, restored.pyro1_mode);
    TEST_ASSERT_EQUAL(original.pyro1_value, restored.pyro1_value);
    TEST_ASSERT_EQUAL(original.pyro2_mode, restored.pyro2_mode);
    TEST_ASSERT_EQUAL(original.pyro2_value, restored.pyro2_value);
    TEST_ASSERT_EQUAL(original.units, restored.units);
    TEST_ASSERT_EQUAL(original.pyro1_refire_speed, restored.pyro1_refire_speed);
    TEST_ASSERT_EQUAL(original.pyro2_refire_speed, restored.pyro2_refire_speed);
    TEST_ASSERT_EQUAL(original.emergency_fire_speed, restored.emergency_fire_speed);
    TEST_ASSERT_EQUAL(original.refire_interval, restored.refire_interval);
    TEST_ASSERT_EQUAL(original.fire_gap, restored.fire_gap);
    TEST_ASSERT_EQUAL(original.log_rate, restored.log_rate);
    TEST_ASSERT_EQUAL(original.lua_enabled, restored.lua_enabled);
}

void test_config_roundtrip_custom(void) {
    config_t original;
    config_set_defaults(&original);

    /* Set every field to a non-default value */
    strncpy(original.id, "CUSTOM01", 8);
    original.id[8] = '\0';
    strncpy(original.name, "TestRkt", 8);
    original.name[8] = '\0';
    original.pyro1_mode = PYRO_MODE_AGL;
    original.pyro1_value = 500;
    original.pyro2_mode = PYRO_MODE_SPEED;
    original.pyro2_value = 42;
    original.units = 2; /* ft */
    original.pyro1_refire_speed = 25;
    original.pyro2_refire_speed = 12;
    original.emergency_fire_speed = 40;
    original.refire_interval = 1500;
    original.fire_gap = 4000;
    original.log_rate = LOG_RATE_EVENTS;
    original.lua_enabled = true;

    char buf[512];
    config_serialize_ini(&original, buf, sizeof(buf));

    config_t restored;
    memset(&restored, 0xFF, sizeof(restored)); /* fill with garbage */
    config_parse_ini(buf, &restored);

    TEST_ASSERT_EQUAL_STRING(original.id, restored.id);
    TEST_ASSERT_EQUAL_STRING(original.name, restored.name);
    TEST_ASSERT_EQUAL(original.pyro1_mode, restored.pyro1_mode);
    TEST_ASSERT_EQUAL(original.pyro1_value, restored.pyro1_value);
    TEST_ASSERT_EQUAL(original.pyro2_mode, restored.pyro2_mode);
    TEST_ASSERT_EQUAL(original.pyro2_value, restored.pyro2_value);
    TEST_ASSERT_EQUAL(original.units, restored.units);
    TEST_ASSERT_EQUAL(original.pyro1_refire_speed, restored.pyro1_refire_speed);
    TEST_ASSERT_EQUAL(original.pyro2_refire_speed, restored.pyro2_refire_speed);
    TEST_ASSERT_EQUAL(original.emergency_fire_speed, restored.emergency_fire_speed);
    TEST_ASSERT_EQUAL(original.refire_interval, restored.refire_interval);
    TEST_ASSERT_EQUAL(original.fire_gap, restored.fire_gap);
    TEST_ASSERT_EQUAL(original.log_rate, restored.log_rate);
    TEST_ASSERT_EQUAL(original.lua_enabled, restored.lua_enabled);
}

/* ── Parser edge cases ────────────────────────────────────────────── */

void test_config_parse_preserves_unset(void) { /* [CFG-06] */
    config_t cfg;
    config_set_defaults(&cfg);
    cfg.pyro1_mode = PYRO_MODE_SPEED;
    cfg.pyro1_value = 99;

    char ini[] = "pyro2_mode=agl\r\npyro2_value=200\r\n";
    config_parse_ini(ini, &cfg);

    /* pyro1 fields unchanged */
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(99, cfg.pyro1_value);
    /* pyro2 fields updated */
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(200, cfg.pyro2_value);
}

void test_config_parse_unknown_keys(void) { /* [CFG-08] */
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "foo=bar\r\npyro1_value=55\r\nbaz=qux\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(55, cfg.pyro1_value);
}

void test_config_parse_comments(void) { /* [CFG-08] */
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "[pyro]\r\n; this is a comment\r\npyro1_value=77\r\n# another comment\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(77, cfg.pyro1_value);
}

void test_config_parse_unix_newlines(void) { /* [CFG-09] */
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "[pyro]\npyro1_mode=speed\npyro1_value=42\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(42, cfg.pyro1_value);
}

void test_config_parse_no_trailing_newline(void) { /* [CFG-09] */
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "pyro1_value=123";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(123, cfg.pyro1_value);
}

void test_config_parse_empty_string(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "";
    config_parse_ini(ini, &cfg);
    /* Should not crash, defaults preserved */
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
}

void test_config_parse_id_truncated(void) { /* [CFG-07] */
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "id=ABCDEFGHIJKLMNOP\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(8, strlen(cfg.id));
}

void test_config_parse_bool_values(void) {
    config_t cfg;
    config_set_defaults(&cfg);

    char ini1[] = "lua_enabled=true\r\n";
    config_parse_ini(ini1, &cfg);
    TEST_ASSERT_TRUE(cfg.lua_enabled);

    char ini2[] = "lua_enabled=0\r\n";
    config_parse_ini(ini2, &cfg);
    TEST_ASSERT_FALSE(cfg.lua_enabled);

    char ini3[] = "lua_enabled=1\r\n";
    config_parse_ini(ini3, &cfg);
    TEST_ASSERT_TRUE(cfg.lua_enabled);
}

void test_config_parse_new_fields(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "landing_timeout=90\r\npyro1_refire_speed=25\r\nemergency_fire_speed=40\r\nfire_gap=4000\r\n"
                 "log_rate=full\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(90, cfg.landing_timeout);
    TEST_ASSERT_EQUAL(25, cfg.pyro1_refire_speed);
    TEST_ASSERT_EQUAL(40, cfg.emergency_fire_speed);
    TEST_ASSERT_EQUAL(4000, cfg.fire_gap);
    TEST_ASSERT_EQUAL(LOG_RATE_FULL, cfg.log_rate);
    char events[] = "log_rate=events\r\n";
    config_parse_ini(events, &cfg);
    TEST_ASSERT_EQUAL(LOG_RATE_EVENTS, cfg.log_rate);
    char junk[] = "log_rate=fast\r\n";
    TEST_ASSERT_EQUAL(1, config_parse_ini(junk, &cfg));
    TEST_ASSERT_EQUAL_MESSAGE(LOG_RATE_EVENTS, cfg.log_rate, "a rate it does not know keeps the one it had");

    /* A config.ini from before DD-062 names a rate: it logs at the default. */
    config_set_defaults(&cfg);
    char old[] = "log_rate_hz=50\r\n";
    config_parse_ini(old, &cfg);
    TEST_ASSERT_EQUAL(LOG_RATE_1HZ, cfg.log_rate);
}

void test_config_parse_all_modes(void) { /* [CFG-04] */
    config_t cfg;
    config_set_defaults(&cfg);

    char ini1[] = "pyro1_mode=delay\r\n";
    config_parse_ini(ini1, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);

    char ini2[] = "pyro1_mode=agl\r\n";
    config_parse_ini(ini2, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro1_mode);

    char ini3[] = "pyro1_mode=fallen\r\n";
    config_parse_ini(ini3, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, cfg.pyro1_mode);

    char ini4[] = "pyro1_mode=speed\r\n";
    config_parse_ini(ini4, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, cfg.pyro1_mode);
}

void test_config_parse_all_units(void) { /* [CFG-03] */
    config_t cfg;
    config_set_defaults(&cfg);

    char ini1[] = "units=cm\r\n";
    config_parse_ini(ini1, &cfg);
    TEST_ASSERT_EQUAL(0, cfg.units);

    char ini2[] = "units=m\r\n";
    config_parse_ini(ini2, &cfg);
    TEST_ASSERT_EQUAL(1, cfg.units);

    char ini3[] = "units=ft\r\n";
    config_parse_ini(ini3, &cfg);
    TEST_ASSERT_EQUAL(2, cfg.units);
}

void test_config_default_ini_string(void) {
    const char *ini = config_default_ini();
    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_TRUE(strlen(ini) > 50);
    /* Should contain key fields */
    TEST_ASSERT_NOT_NULL(strstr(ini, "pyro1_mode=delay"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "pyro2_mode=agl"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "landing_timeout=60"));
}

/* ── A disabled channel stays disabled [CFG-04] ───────────────────
 *
 * POST /api/config parses the posted keys over the running config and writes
 * the re-serialised result, so every mode has to survive that trip: delay 0
 * fires at apogee. */

void test_config_mode_none_round_trips(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "[pyro]\r\npyro1_mode=none\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_NONE, cfg.pyro1_mode);

    char buf[512];
    TEST_ASSERT_GREATER_THAN(0, config_serialize_ini(&cfg, buf, (int)sizeof(buf)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "pyro1_mode=none"), buf);

    config_t back;
    config_set_defaults(&back);
    config_parse_ini(buf, &back);
    TEST_ASSERT_EQUAL_MESSAGE(PYRO_MODE_NONE, back.pyro1_mode, "a disabled channel came back armed");
}

void test_config_unknown_mode_serialises_as_none(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    cfg.pyro2_mode = 9; /* no such mode */
    char buf[512];
    TEST_ASSERT_GREATER_THAN(0, config_serialize_ini(&cfg, buf, (int)sizeof(buf)));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "pyro2_mode=none"),
                                 "an unknown mode must be written as one that never fires");
}

/* ── The shipped defaults fit their fields [CFG-07] ───────────────── */

void test_config_default_name_is_not_truncated(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    TEST_ASSERT_EQUAL_STRING("MyRocket", cfg.name);
}

/* ── Every key written is one something reads [CFG-SUBSYS-01] ───── */

/* [CFG-SUBSYS-01] */
void test_config_writes_no_inert_keys(void) {
    const char *ini = config_default_ini();
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "beep_mode="), "beep_mode is read by nothing");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "max_coast_s="), "max_coast_s is read by nothing (DD-022)");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "log_enabled="), "log_enabled is read by nothing");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "buzzer_startup="), "buzzer_startup is read by nothing");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "telem_rate_hz="), "telemetry is 1 Hz [TEL-03, DD-088]");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "telem_format="), "telemetry is $PYRO [TEL-01, DD-088]");
    TEST_ASSERT_NOT_NULL(strstr(ini, "emergency_fire_speed=0\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "log_rate=1hz\r\n"));
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "log_rate_hz="), "replaced by log_rate [DD-064]");
    TEST_ASSERT_NULL_MESSAGE(strstr(ini, "log_high_rate="), "replaced by log_rate [DD-064]");
}

/* ── CFG-06: a partial file must not reset what it omits ──────────── */

/* This is the composition POST /api/config performs: take the running config,
 * parse the partial body over it, re-serialise the result. Writing the body
 * verbatim instead is what let the Config tab wipe the Lua pin roles and the
 * Lua tab reset the rocket id and both pyro modes. */
/* The CFG-06 merge, as http_server.c performs it: parse the partial body over
 * the RUNNING config, then serialise.
 *
 * `out` is the in-memory result and `ser`, when given, receives the text that
 * would be written. They differ for the legacy lua_p* keys: those are parsed
 * and kept in memory but never written, so a board that still has them
 * migrates correctly and then sheds them on its first save. See
 * config_fields.h. */
static void merge_partial_ser(const config_t *running, const char *partial, config_t *out, char *ser, int ser_len) {
    char buf[512];
    char scratch[512];
    *out = *running;
    strncpy(buf, partial, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    config_parse_ini(buf, out);
    int n = config_serialize_ini(out, ser ? ser : scratch, ser ? ser_len : (int)sizeof(scratch));
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, n, "the merged config must fit the budget");
}

static void merge_partial(const config_t *running, const char *partial, config_t *out) {
    merge_partial_ser(running, partial, out, NULL, 0);
}

void test_config_merge_keeps_omitted_fields(void) {
    config_t running;
    config_set_defaults(&running);
    strncpy(running.id, "ROCKET7", sizeof(running.id) - 1);
    running.lua_enabled = true;
    running.lua_baud = 19200;
    running.pyro2_value = 250;

    /* What the Config tab posts: its own keys only, no lua_*. */
    config_t merged;
    merge_partial(&running, "[pyro]\r\nid=ROCKET9\r\npyro1_value=7\r\n", &merged);

    TEST_ASSERT_EQUAL_STRING("ROCKET9", merged.id);   /* changed  */
    TEST_ASSERT_EQUAL(7, merged.pyro1_value);         /* changed  */
    /* The keys the Lua tab owns survive a Config-tab save. */
    TEST_ASSERT_TRUE(merged.lua_enabled);
    TEST_ASSERT_EQUAL(19200, merged.lua_baud);
    TEST_ASSERT_EQUAL(250, merged.pyro2_value);           /* survived */
}

void test_config_merge_lua_tab_keeps_flight_fields(void) {
    config_t running;
    config_set_defaults(&running);
    strncpy(running.id, "ROCKET7", sizeof(running.id) - 1);
    running.pyro1_mode = PYRO_MODE_SPEED;
    running.pyro1_value = 42;

    /* What the Lua tab posts: lua_* only. */
    config_t merged;
    merge_partial(&running, "[pyro]\r\nlua_enabled=true\r\nlua_baud=57600\r\n", &merged);

    TEST_ASSERT_TRUE(merged.lua_enabled);      /* changed */
    TEST_ASSERT_EQUAL(57600, merged.lua_baud); /* changed */
    TEST_ASSERT_EQUAL_STRING("ROCKET7", merged.id);        /* survived */
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, merged.pyro1_mode); /* survived */
    TEST_ASSERT_EQUAL(42, merged.pyro1_value);             /* survived */
}

/* ── The budget of config.ini ─────────────────────────────────────
 *
 * Every reader and writer of the file holds CONFIG_INI_MAX bytes, so the
 * worst case must fit it with room for the next field. */

void test_config_worst_case_fits_the_budget(void) {
    config_t cfg;
    config_set_defaults(&cfg);

    /* Every string at its 8-character maximum, every number at its widest,
       every mode at its longest name. */
    strncpy(cfg.id, "88888888", sizeof(cfg.id) - 1);
    strncpy(cfg.name, "88888888", sizeof(cfg.name) - 1);
    cfg.pyro1_mode = PYRO_MODE_FALLEN;
    cfg.pyro2_mode = PYRO_MODE_FALLEN;
    cfg.pyro1_value = 65535;
    cfg.pyro2_value = 65535;
    cfg.units = 0; /* "cm" is shortest, but units is not the driver here */
    cfg.pyro1_refire_speed = 65535;
    cfg.pyro2_refire_speed = 65535;
    cfg.emergency_fire_speed = 65535;
    cfg.refire_interval = 65535;
    cfg.fire_gap = 65535;
    cfg.log_rate = LOG_RATE_EVENTS; /* the longest name */
    cfg.landing_timeout = 255;
    cfg.lua_baud = 921600;
    cfg.lua_pixels = 65535;
    char buf[CONFIG_INI_MAX];
    int n = config_serialize_ini(&cfg, buf, (int)sizeof(buf));
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, n, "a fully populated config must still serialise");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(CONFIG_INI_MAX - 64, n, "config.ini is running out of headroom");
}

/* ── Ranges [SYS-CFG-03] ───────────────────────────────────────────
 *
 * A value outside its row's range, or one that does not parse, is refused and
 * the field keeps what it had. Cast instead, 700 m entered as 70000 cm became
 * 4464 cm. */

void test_SYS_CFG_03_a_value_beyond_its_field_is_refused_not_wrapped(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "units=cm\r\npyro2_value=70000\r\n";
    TEST_ASSERT_EQUAL(1, config_parse_ini(ini, &cfg));
    TEST_ASSERT_EQUAL_MESSAGE(300, cfg.pyro2_value, "70000 cm wrapped instead of being refused");
    TEST_ASSERT_EQUAL(UNITS_CM, cfg.units);

    char top[] = "pyro2_value=65535\r\n";
    TEST_ASSERT_EQUAL(0, config_parse_ini(top, &cfg));
    TEST_ASSERT_EQUAL(65535, cfg.pyro2_value);
}

void test_SYS_CFG_03_a_value_that_does_not_parse_keeps_the_previous(void) {
    static const char *bad[] = {"pyro1_value=abc", "pyro1_value=-5", "pyro1_value=12x", "pyro1_value=",
                                "pyro1_value=+7", "pyro1_value=99999999999999999999"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        config_t cfg;
        config_set_defaults(&cfg);
        cfg.pyro1_value = 42;
        char ini[64];
        snprintf(ini, sizeof(ini), "%s\r\n", bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(1, config_parse_ini(ini, &cfg), bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(42, cfg.pyro1_value, bad[i]);
    }
}

void test_SYS_CFG_03_every_bounded_field_refuses_beyond_its_row(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "landing_timeout=256\r\nlua_baud=299\r\n"
                 "lua_baud=921601\r\nlua_enabled=yes\r\nunits=furlongs\r\n";
    TEST_ASSERT_EQUAL(5, config_parse_ini(ini, &cfg));
    config_t def;
    config_set_defaults(&def);
    TEST_ASSERT_EQUAL(0, memcmp(&def, &cfg, sizeof(cfg)));
}

/* [CFG-04] A mode it cannot name is none, and still counted as refused. */
void test_CFG_04_an_unnamed_mode_is_none_and_reported(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "pyro1_mode=apogee\r\n";
    TEST_ASSERT_EQUAL(1, config_parse_ini(ini, &cfg));
    TEST_ASSERT_EQUAL(PYRO_MODE_NONE, cfg.pyro1_mode);
}

/* ── One tokenizer [CFG-09] ─────────────────────────────────────── */

void test_CFG_09_blanks_around_key_and_value_are_not_part_of_them(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    cfg.pyro1_mode = PYRO_MODE_NONE;
    char ini[] = "pyro1_mode=delay \r\npyro2_value = 150\r\n\tunits\t=\tft\t\n  [pyro]\n  ; note\nname = Rkt 7 \n";
    TEST_ASSERT_EQUAL(0, config_parse_ini(ini, &cfg));
    TEST_ASSERT_EQUAL_MESSAGE(PYRO_MODE_DELAY, cfg.pyro1_mode, "a trailing blank turned delay into none");
    TEST_ASSERT_EQUAL_MESSAGE(150, cfg.pyro2_value, "a blank before '=' hid the key");
    TEST_ASSERT_EQUAL(UNITS_FT, cfg.units);
    TEST_ASSERT_EQUAL_STRING("Rkt 7", cfg.name);
}

void test_CFG_09_a_line_with_no_key_carries_nothing(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "=5\r\n  =  \r\npyro1_value\r\n";
    TEST_ASSERT_EQUAL(0, config_parse_ini(ini, &cfg));
    config_t def;
    config_set_defaults(&def);
    TEST_ASSERT_EQUAL(0, memcmp(&def, &cfg, sizeof(cfg)));
}

void test_lua_baud_holds_115200(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "lua_baud=115200\r\n";
    TEST_ASSERT_EQUAL(0, config_parse_ini(ini, &cfg));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(115200u, cfg.lua_baud, "115200 wrapped to 49664");

    char buf[CONFIG_INI_MAX];
    TEST_ASSERT_GREATER_THAN(0, config_serialize_ini(&cfg, buf, (int)sizeof(buf)));
    config_t back;
    config_set_defaults(&back);
    config_parse_ini(buf, &back);
    TEST_ASSERT_EQUAL_UINT32(115200u, back.lua_baud);
}

/* ── Reading config.ini [CFG-05, FLT-BOOT-18] ─────────────────────
 *
 * Only a file that does not exist is replaced by the defaults. A read that
 * failed -- an I/O error, a lock not had in time, the flight log holding the
 * filesystem -- leaves the file as it is for the next boot. */

void test_CFG_05_only_a_missing_file_is_rewritten(void) {
    static const int failures[] = {HAL_FS_ERROR, HAL_FS_LOCKED};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        config_t cfg;
        char buf[CONFIG_INI_MAX];
        TEST_ASSERT_EQUAL(CONFIG_FILE_UNREADABLE, config_from_file(&cfg, buf, (int)sizeof(buf), failures[i], NULL));
        TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
    }
    config_t cfg;
    char buf[CONFIG_INI_MAX];
    TEST_ASSERT_EQUAL(CONFIG_FILE_MISSING, config_from_file(&cfg, buf, (int)sizeof(buf), HAL_FS_NOENT, NULL));
    TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
}

void test_CFG_05_a_file_that_filled_the_buffer_is_not_parsed(void) {
    config_t cfg;
    char buf[64];
    memset(buf, ';', sizeof(buf));
    memcpy(buf, "pyro2_value=1", 13); /* "...=150", cut */
    TEST_ASSERT_EQUAL(CONFIG_FILE_UNREADABLE, config_from_file(&cfg, buf, (int)sizeof(buf), (int)sizeof(buf) - 1, NULL));
    TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
}

/* Comments make a hand-edited config.ini longer than the 511 bytes once read. */
void test_CFG_05_a_long_commented_file_is_read_whole(void) {
    char file[CONFIG_INI_MAX];
    int n = snprintf(file, sizeof(file), "[pyro]\r\n");
    for (int i = 0; i < 12; i++)
        n += snprintf(file + n, sizeof(file) - (size_t)n, "; a note an operator wrote about the rocket, line %02d\r\n", i);
    n += snprintf(file + n, sizeof(file) - (size_t)n, "pyro2_value=150\r\n");
    TEST_ASSERT_GREATER_THAN(600, n);

    config_t cfg;
    int rejected = -1;
    TEST_ASSERT_EQUAL(CONFIG_FILE_LOADED, config_from_file(&cfg, file, (int)sizeof(file), n, &rejected));
    TEST_ASSERT_EQUAL(0, rejected);
    TEST_ASSERT_EQUAL(150, cfg.pyro2_value);
}

/* ── Serializer overflow ──────────────────────────────────────────── */

/* snprintf reports what it would have written, so an unguarded `pos += n`
 * runs past the buffer and hands the next call a negative size. */
void test_config_serialize_refuses_to_overflow(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    /* A guard region after the buffer the serialiser is allowed to use.
     * snprintf may write a terminator anywhere inside its own size, so the
     * property under test is that nothing lands beyond it. */
    char arena[96];
    const int usable = 32;
    memset(arena, 0x7f, sizeof(arena));
    TEST_ASSERT_LESS_OR_EQUAL(0, config_serialize_ini(&cfg, arena, usable));
    for (size_t i = (size_t)usable; i < sizeof(arena); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x7f, (unsigned char)arena[i]);
    }
}

int main(void) {
    UNITY_BEGIN();

    /* Defaults */
    RUN_TEST(test_config_defaults);

    /* Round-trip [CFG-TABLE-02] */
    RUN_TEST(test_config_roundtrip_defaults);
    RUN_TEST(test_config_roundtrip_custom);

    /* Parser edge cases */
    RUN_TEST(test_config_parse_preserves_unset);
    RUN_TEST(test_config_parse_unknown_keys);
    RUN_TEST(test_config_parse_comments);
    RUN_TEST(test_config_parse_unix_newlines);
    RUN_TEST(test_config_parse_no_trailing_newline);
    RUN_TEST(test_config_parse_empty_string);
    RUN_TEST(test_config_parse_id_truncated);
    RUN_TEST(test_config_parse_bool_values);
    RUN_TEST(test_config_parse_new_fields);
    RUN_TEST(test_config_parse_all_modes);
    RUN_TEST(test_config_parse_all_units);

    /* Merge semantics [CFG-06] */
    RUN_TEST(test_config_merge_keeps_omitted_fields);
    RUN_TEST(test_config_worst_case_fits_the_budget);
    RUN_TEST(test_config_merge_lua_tab_keeps_flight_fields);
    RUN_TEST(test_config_serialize_refuses_to_overflow);

    /* Default INI string */
    RUN_TEST(test_config_default_ini_string);

    RUN_TEST(test_config_mode_none_round_trips);
    RUN_TEST(test_config_unknown_mode_serialises_as_none);
    RUN_TEST(test_config_default_name_is_not_truncated);
    RUN_TEST(test_config_writes_no_inert_keys);

    RUN_TEST(test_SYS_CFG_03_a_value_beyond_its_field_is_refused_not_wrapped);
    RUN_TEST(test_SYS_CFG_03_a_value_that_does_not_parse_keeps_the_previous);
    RUN_TEST(test_SYS_CFG_03_every_bounded_field_refuses_beyond_its_row);
    RUN_TEST(test_CFG_04_an_unnamed_mode_is_none_and_reported);
    RUN_TEST(test_CFG_09_blanks_around_key_and_value_are_not_part_of_them);
    RUN_TEST(test_CFG_09_a_line_with_no_key_carries_nothing);
    RUN_TEST(test_lua_baud_holds_115200);
    RUN_TEST(test_CFG_05_only_a_missing_file_is_rewritten);
    RUN_TEST(test_CFG_05_a_file_that_filled_the_buffer_is_not_parsed);
    RUN_TEST(test_CFG_05_a_long_commented_file_is_read_whole);

    return UNITY_END();
}
