/*
 * Config module tests — round-trip, defaults, parser, serializer.
 * [CFG-TABLE-02] Every field survives serialize → parse.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "config.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Defaults ─────────────────────────────────────────────────────── */

void test_config_defaults(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    TEST_ASSERT_EQUAL_STRING("PYRO001", cfg.id);
    TEST_ASSERT_EQUAL_STRING("My Rocke", cfg.name); /* truncated to 8 chars */
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(0, cfg.pyro1_value);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
    TEST_ASSERT_EQUAL(1, cfg.units); /* meters */
    TEST_ASSERT_EQUAL(0, cfg.beep_mode);
    TEST_ASSERT_EQUAL(30, cfg.max_coast_s);
    TEST_ASSERT_EQUAL(0, cfg.telem_format);
    TEST_ASSERT_EQUAL(10, cfg.telem_rate_hz);
    TEST_ASSERT_EQUAL(50, cfg.log_rate_hz);
    TEST_ASSERT_TRUE(cfg.log_enabled);
    TEST_ASSERT_TRUE(cfg.buzzer_startup);
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
    TEST_ASSERT_EQUAL(original.beep_mode, restored.beep_mode);
    TEST_ASSERT_EQUAL(original.max_coast_s, restored.max_coast_s);
    TEST_ASSERT_EQUAL(original.telem_format, restored.telem_format);
    TEST_ASSERT_EQUAL(original.telem_rate_hz, restored.telem_rate_hz);
    TEST_ASSERT_EQUAL(original.log_rate_hz, restored.log_rate_hz);
    TEST_ASSERT_EQUAL(original.log_enabled, restored.log_enabled);
    TEST_ASSERT_EQUAL(original.buzzer_startup, restored.buzzer_startup);
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
    original.beep_mode = 1;
    original.max_coast_s = 120;
    original.telem_format = 1;
    original.telem_rate_hz = 5;
    original.log_rate_hz = 25;
    original.log_enabled = false;
    original.buzzer_startup = false;

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
    TEST_ASSERT_EQUAL(original.beep_mode, restored.beep_mode);
    TEST_ASSERT_EQUAL(original.max_coast_s, restored.max_coast_s);
    TEST_ASSERT_EQUAL(original.telem_format, restored.telem_format);
    TEST_ASSERT_EQUAL(original.telem_rate_hz, restored.telem_rate_hz);
    TEST_ASSERT_EQUAL(original.log_rate_hz, restored.log_rate_hz);
    TEST_ASSERT_EQUAL(original.log_enabled, restored.log_enabled);
    TEST_ASSERT_EQUAL(original.buzzer_startup, restored.buzzer_startup);
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

    char ini1[] = "log_enabled=false\r\nbuzzer_startup=0\r\n";
    config_parse_ini(ini1, &cfg);
    TEST_ASSERT_FALSE(cfg.log_enabled);
    TEST_ASSERT_FALSE(cfg.buzzer_startup);

    char ini2[] = "log_enabled=true\r\nbuzzer_startup=1\r\n";
    config_parse_ini(ini2, &cfg);
    TEST_ASSERT_TRUE(cfg.log_enabled);
    TEST_ASSERT_TRUE(cfg.buzzer_startup);
}

void test_config_parse_new_fields(void) {
    config_t cfg;
    config_set_defaults(&cfg);
    char ini[] = "max_coast_s=60\r\ntelem_rate_hz=5\r\nlog_rate_hz=25\r\n";
    config_parse_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(60, cfg.max_coast_s);
    TEST_ASSERT_EQUAL(5, cfg.telem_rate_hz);
    TEST_ASSERT_EQUAL(25, cfg.log_rate_hz);
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
    TEST_ASSERT_NOT_NULL(strstr(ini, "max_coast_s=30"));
}

/* ── Test runner ──────────────────────────────────────────────────── */


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

/* ── The 512-byte budget ──────────────────────────────────────────
 *
 * hal_config_load() reads into char[512] and http_server.c refuses a merged
 * config that does not fit. Before the legacy keys were dropped from the
 * output a fully populated config serialised to about 525 bytes, so a board
 * with every Lua name filled in could not save at all and answered HTTP 500.
 * Nothing tested the worst case, only the defaults. */

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
    cfg.beep_mode = 255;
    cfg.max_coast_s = 255;
    cfg.telem_format = 255;
    cfg.telem_rate_hz = 255;
    cfg.log_rate_hz = 255;
    cfg.landing_timeout = 255;
    cfg.lua_baud = 65535;
    cfg.lua_pixels = 65535;
    char buf[512];
    int n = config_serialize_ini(&cfg, buf, (int)sizeof(buf));
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, n, "a fully populated config must still serialise");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(512, n, "and must fit what hal_config_load() reads back");
    /* Headroom, so the next field added does not silently land on the limit.
       341 bytes today; this fires well before 512. */
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(440, n, "config.ini is running out of headroom");
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

    return UNITY_END();
}
