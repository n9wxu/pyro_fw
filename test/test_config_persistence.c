/*
 * The stored configuration and when it takes effect [SYS-CFG-01, CFG-01,
 * CFG-04, CFG-05, CFG-10].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "../src/config.h"
#include "../src/flight_states.h"
#include "../src/hal.h"

void setUp(void) {
    mock_reset_all();
}

void tearDown(void) {}

static flight_context_t ctx;

static void start(void) {
    memset(&ctx, 0, sizeof(ctx));
    flight_init(&ctx);
}

static void run_to(flight_state_t state) {
    for (uint32_t t = mock_time_ms; ctx.current_state != state && t < mock_time_ms + 30000u; t++) {
        mock_time_ms = t;
        hal_tasks_tick(t);
        ctx.current_state = dispatch_state(&ctx, t);
    }
    TEST_ASSERT_EQUAL(state, ctx.current_state);
}

static void store(const char *ini) {
    TEST_ASSERT_EQUAL(0, hal_fs_write_file("config.ini", ini, (int)strlen(ini)));
}

/* ── Stored across a power cycle [SYS-CFG-01, CFG-01] ─────────────── */

void test_SYS_CFG_01_a_saved_configuration_is_read_back(void) {
    config_t saved;
    config_set_defaults(&saved);
    strncpy(saved.id, "TEST001", sizeof(saved.id) - 1);
    saved.pyro1_mode = PYRO_MODE_FALLEN;
    saved.pyro1_value = 100;
    saved.units = 2;
    saved.emergency_fire_speed = 40;
    saved.fire_gap = 4500;
    TEST_ASSERT_EQUAL(0, hal_config_save(&saved));

    config_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT_EQUAL(0, hal_config_load(&loaded));
    TEST_ASSERT_EQUAL_STRING("TEST001", loaded.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, loaded.pyro1_mode);
    TEST_ASSERT_EQUAL(100, loaded.pyro1_value);
    TEST_ASSERT_EQUAL(2, loaded.units);
    TEST_ASSERT_EQUAL(40, loaded.emergency_fire_speed);
    TEST_ASSERT_EQUAL(4500, loaded.fire_gap);
}

void test_CFG_01_the_file_is_ini_text(void) {
    config_t saved;
    config_set_defaults(&saved);
    saved.pyro2_value = 250;
    TEST_ASSERT_EQUAL(0, hal_config_save(&saved));
    char text[CONFIG_INI_MAX];
    int n = mock_fs_peek("config.ini", text, (int)sizeof(text) - 1);
    TEST_ASSERT_TRUE(n > 0);
    text[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(text, "[pyro]"));
    TEST_ASSERT_NOT_NULL(strstr(text, "pyro2_value=250\r\n"));
}

void test_SYS_CFG_01_each_save_replaces_the_last(void) {
    config_t c;
    config_set_defaults(&c);
    c.pyro1_value = 100;
    hal_config_save(&c);
    hal_config_load(&c);
    c.pyro1_value = 200;
    c.pyro2_value = 300;
    hal_config_save(&c);
    hal_config_load(&c);
    c.pyro1_value = 400;
    hal_config_save(&c);

    config_t last;
    hal_config_load(&last);
    TEST_ASSERT_EQUAL(400, last.pyro1_value);
    TEST_ASSERT_EQUAL(300, last.pyro2_value);
}

/* ── What a start makes of the file [CFG-04, CFG-05] ──────────────── */

void test_CFG_05_a_board_with_no_file_starts_on_the_defaults_and_stores_them(void) {
    start();
    config_t defaults;
    config_set_defaults(&defaults);
    TEST_ASSERT_EQUAL_STRING(defaults.id, ctx.config.id);
    TEST_ASSERT_EQUAL(defaults.pyro1_mode, ctx.config.pyro1_mode);
    TEST_ASSERT_EQUAL(defaults.pyro2_value, ctx.config.pyro2_value);
    char text[CONFIG_INI_MAX];
    TEST_ASSERT_TRUE_MESSAGE(mock_fs_peek("config.ini", text, (int)sizeof(text)) > 0, "the default file is created");
}

/* A mode the board cannot name is a channel that never fires. */
void test_CFG_04_a_mode_it_cannot_name_is_none(void) {
    store("[pyro]\npyro1_mode=sideways\npyro2_mode=agl\npyro2_value=200\n");
    start();
    run_to(PAD_IDLE);
    TEST_ASSERT_EQUAL(PYRO_MODE_NONE, ctx.config.pyro1_mode);
    TEST_ASSERT_FALSE(ctx.plan.enabled[0]);
    TEST_ASSERT_TRUE(ctx.plan.enabled[1]);
}

void test_CFG_03_units_it_cannot_name_are_metres(void) {
    store("[pyro]\nunits=furlongs\n");
    start();
    TEST_ASSERT_EQUAL(1, ctx.config.units);
}

/* ── Changes take effect at start-up [CFG-10] ─────────────────────── */

void test_CFG_10_the_running_system_keeps_the_configuration_it_started_with(void) {
    store("[pyro]\nid=FIRST\npyro1_mode=delay\npyro2_mode=agl\npyro2_value=300\n");
    start();
    run_to(PAD_IDLE);
    store("[pyro]\nid=SECOND\npyro1_mode=none\npyro2_mode=agl\npyro2_value=100\n");
    for (uint32_t end = mock_time_ms + 3000u; mock_time_ms < end; mock_time_ms++) {
        hal_tasks_tick(mock_time_ms);
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL_STRING("FIRST", ctx.config.id);
    TEST_ASSERT_TRUE(ctx.plan.enabled[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, ctx.plan.trigger_m[1]);

    start();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("SECOND", ctx.config.id, "the next start takes the change");
    TEST_ASSERT_EQUAL(PYRO_MODE_NONE, ctx.config.pyro1_mode);
}

/* ── The context other modules read ───────────────────────────────── */

void test_the_published_context_is_the_running_one(void) {
    start();
    TEST_ASSERT_EQUAL_PTR(&ctx, flight_get_context());
    TEST_ASSERT_EQUAL(BOOT_SETTLE, flight_get_state());
    run_to(PAD_IDLE);
    TEST_ASSERT_EQUAL(PAD_IDLE, flight_get_state());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SYS_CFG_01_a_saved_configuration_is_read_back);
    RUN_TEST(test_CFG_01_the_file_is_ini_text);
    RUN_TEST(test_SYS_CFG_01_each_save_replaces_the_last);
    RUN_TEST(test_CFG_05_a_board_with_no_file_starts_on_the_defaults_and_stores_them);
    RUN_TEST(test_CFG_04_a_mode_it_cannot_name_is_none);
    RUN_TEST(test_CFG_03_units_it_cannot_name_are_metres);
    RUN_TEST(test_CFG_10_the_running_system_keeps_the_configuration_it_started_with);
    RUN_TEST(test_the_published_context_is_the_running_one);
    return UNITY_END();
}
