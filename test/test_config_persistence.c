/*
 * Config persistence, and the runtime reload that only the pad allows.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/pressure_processing.h"
#include "../src/flight_states.h"
#include "../src/hal.h"
#include "../src/config.h"
#include "mocks.h"
#include <string.h>

void setUp(void) {
    mock_reset_all();
}

void tearDown(void) {}

/* Boot to PAD_IDLE through the HAL's ticks: the mock sensor feeds the
 * pressure layer from hal_tasks_tick(), and calibration needs its samples. */
static void advance_to_pad_idle(flight_context_t *ctx) {
    mock_time_ms = 0;
    while (ctx->current_state != PAD_IDLE && mock_time_ms < 30000) {
        hal_tasks_tick(mock_time_ms);
        ctx->current_state = dispatch_state(ctx, mock_time_ms);
        mock_time_ms += 100;
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx->current_state);
}

static void save_config_named(const char *id) {
    config_t cfg;
    config_set_defaults(&cfg);
    strncpy(cfg.id, id, 8);
    cfg.id[8] = '\0';
    hal_config_save(&cfg);
}

/* [FLT-BOOT-02] */
void test_config_persists_across_power_cycle(void) {
    config_t cfg1;
    config_set_defaults(&cfg1);
    strncpy(cfg1.id, "TEST001", 8);
    cfg1.id[8] = '\0';
    cfg1.pyro1_mode = PYRO_MODE_FALLEN;
    cfg1.pyro1_value = 100;
    cfg1.units = 2;
    TEST_ASSERT_EQUAL(0, hal_config_save(&cfg1));

    config_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    TEST_ASSERT_EQUAL(0, hal_config_load(&cfg2));
    TEST_ASSERT_EQUAL_STRING("TEST001", cfg2.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, cfg2.pyro1_mode);
    TEST_ASSERT_EQUAL(100, cfg2.pyro1_value);
    TEST_ASSERT_EQUAL(2, cfg2.units);
}

void test_config_reload_succeeds_in_pad_idle(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    advance_to_pad_idle(&ctx);
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, ctx.config.pyro1_mode);

    config_t new_cfg;
    config_set_defaults(&new_cfg);
    strncpy(new_cfg.id, "RELOAD1", 8);
    new_cfg.id[8] = '\0';
    new_cfg.pyro1_mode = PYRO_MODE_AGL;
    new_cfg.pyro1_value = 500;
    hal_config_save(&new_cfg);

    TEST_ASSERT_EQUAL(CFG_APPLIED, flight_config_reload(&ctx));
    TEST_ASSERT_EQUAL_STRING("RELOAD1", ctx.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, ctx.config.pyro1_mode);
    TEST_ASSERT_EQUAL(500, ctx.config.pyro1_value);
}

/* Off the pad the running config is the flight's, in every state. */
static void assert_reload_refused_in(flight_state_t st) {
    flight_context_t ctx;
    flight_init(&ctx);
    ctx.current_state = st;
    save_config_named("UNSAFE");
    TEST_ASSERT_EQUAL(CFG_NOT_ON_PAD, flight_config_reload(&ctx));
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
}

void test_config_reload_rejected_during_ascent(void) {
    assert_reload_refused_in(ASCENT);
}

void test_config_reload_rejected_during_descent(void) {
    assert_reload_refused_in(FALLING);
}

void test_config_reload_rejected_during_landed(void) {
    assert_reload_refused_in(LANDED);
}

/* An out-of-range mode cannot reach the reload's validation through a file:
 * config_serialize_ini() writes config_mode_name(99), "none", and the parser
 * maps any unrecognised name to NONE. So the property is stronger than a
 * rejection: the board ends with a valid config, and the channel it could not
 * name never fires. */
void test_config_reload_normalises_invalid_pyro_mode(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    advance_to_pad_idle(&ctx);

    config_t bad_cfg;
    config_set_defaults(&bad_cfg);
    bad_cfg.pyro1_mode = 99;
    hal_config_save(&bad_cfg);

    TEST_ASSERT_EQUAL(CFG_APPLIED, flight_config_reload(&ctx));
    TEST_ASSERT_EQUAL(PYRO_MODE_NONE, ctx.config.pyro1_mode);
}

/* The in-memory backstop the file path cannot reach. */
void test_config_apply_rejects_an_invalid_config(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    advance_to_pad_idle(&ctx);
    config_t bad_cfg;
    config_set_defaults(&bad_cfg);
    bad_cfg.pyro2_mode = 99;
    TEST_ASSERT_EQUAL(CFG_INVALID, flight_config_apply(&ctx, &bad_cfg));
    bad_cfg.pyro2_mode = PYRO_MODE_NONE;
    bad_cfg.units = 3;
    TEST_ASSERT_EQUAL(CFG_INVALID, flight_config_apply(&ctx, &bad_cfg));
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
}

void test_config_reload_fails_if_file_missing(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    advance_to_pad_idle(&ctx);
    mock_reset_all(); /* the filesystem with it */
    TEST_ASSERT_EQUAL(CFG_LOAD_FAILED, flight_config_reload(&ctx));
}

void test_multiple_config_changes_persist(void) {
    config_t cfg1;
    config_set_defaults(&cfg1);
    cfg1.pyro1_value = 100;
    hal_config_save(&cfg1);

    config_t cfg2;
    config_set_defaults(&cfg2);
    hal_config_load(&cfg2);
    cfg2.pyro1_value = 200;
    cfg2.pyro2_value = 300;
    hal_config_save(&cfg2);

    config_t cfg3;
    config_set_defaults(&cfg3);
    hal_config_load(&cfg3);
    cfg3.pyro1_value = 400;
    hal_config_save(&cfg3);

    config_t final;
    hal_config_load(&final);
    TEST_ASSERT_EQUAL(400, final.pyro1_value);
    TEST_ASSERT_EQUAL(300, final.pyro2_value);
}

void test_config_survives_flight_cycle(void) {
    config_t pre_flight;
    config_set_defaults(&pre_flight);
    strncpy(pre_flight.id, "FLIGHT1", 8);
    pre_flight.id[8] = '\0';
    pre_flight.pyro1_mode = PYRO_MODE_AGL;
    pre_flight.pyro1_value = 300;
    hal_config_save(&pre_flight);

    flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_EQUAL_STRING("FLIGHT1", ctx.config.id);

    mock_reset_all(); /* a new boot; the file is put back, as flash keeps it */
    hal_config_save(&pre_flight);

    flight_context_t ctx2;
    flight_init(&ctx2);
    TEST_ASSERT_EQUAL_STRING("FLIGHT1", ctx2.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, ctx2.config.pyro1_mode);
    TEST_ASSERT_EQUAL(300, ctx2.config.pyro1_value);
}

/* As the pyro mode: units_to_str(5) writes "m", and the parser maps anything
 * unrecognised to 1. */
void test_config_reload_normalises_invalid_units(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    advance_to_pad_idle(&ctx);

    config_t bad_cfg;
    config_set_defaults(&bad_cfg);
    bad_cfg.units = 5;
    hal_config_save(&bad_cfg);

    TEST_ASSERT_EQUAL(CFG_APPLIED, flight_config_reload(&ctx));
    TEST_ASSERT_EQUAL(1, ctx.config.units);
}

void test_flight_get_context_returns_correct_pointer(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_EQUAL_PTR(&ctx, flight_get_context());
}

void test_flight_get_state_returns_correct_state(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_EQUAL(BOOT_SETTLE, flight_get_state());
    advance_to_pad_idle(&ctx);
    TEST_ASSERT_EQUAL(PAD_IDLE, flight_get_state());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_config_persists_across_power_cycle);
    RUN_TEST(test_config_reload_succeeds_in_pad_idle);
    RUN_TEST(test_config_reload_rejected_during_ascent);
    RUN_TEST(test_config_reload_rejected_during_descent);
    RUN_TEST(test_config_reload_rejected_during_landed);
    RUN_TEST(test_config_reload_normalises_invalid_pyro_mode);
    RUN_TEST(test_config_apply_rejects_an_invalid_config);
    RUN_TEST(test_config_reload_fails_if_file_missing);
    RUN_TEST(test_multiple_config_changes_persist);
    RUN_TEST(test_config_survives_flight_cycle);
    RUN_TEST(test_config_reload_normalises_invalid_units);
    RUN_TEST(test_flight_get_context_returns_correct_pointer);
    RUN_TEST(test_flight_get_state_returns_correct_state);
    return UNITY_END();
}
