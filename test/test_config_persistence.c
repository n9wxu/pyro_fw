/*
 * Config persistence and runtime reload tests.
 * Verifies config save/load cycle and runtime reload safety.
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/flight_states.h"
#include "../src/hal.h"
#include "../src/config.h"
#include "mocks.h"
#include <string.h>

void setUp(void) {
    mock_reset_all();
}

void tearDown(void) {}

/* ── Test 1: Config persists across power cycle (save → load) ── */
void test_config_persists_across_power_cycle(void) {
    /* Simulate first boot: create custom config and save */
    config_t cfg1;
    config_set_defaults(&cfg1);
    strncpy(cfg1.id, "TEST001", 8);
    cfg1.id[8] = '\0';
    cfg1.pyro1_mode = PYRO_MODE_FALLEN;
    cfg1.pyro1_value = 100;
    cfg1.units = 2; /* ft */

    int save_result = hal_config_save(&cfg1);
    TEST_ASSERT_EQUAL(0, save_result);

    /* Simulate power cycle: load config into new struct */
    config_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    int load_result = hal_config_load(&cfg2);
    TEST_ASSERT_EQUAL(0, load_result);

    /* Verify all fields persisted */
    TEST_ASSERT_EQUAL_STRING("TEST001", cfg2.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, cfg2.pyro1_mode);
    TEST_ASSERT_EQUAL(100, cfg2.pyro1_value);
    TEST_ASSERT_EQUAL(2, cfg2.units);
}

/* ── Test 2: Runtime reload succeeds in PAD_IDLE ── */
void test_config_reload_succeeds_in_pad_idle(void) {
    /* Initialize flight context in PAD_IDLE */
    flight_context_t ctx;
    flight_init(&ctx);

    /* Advance to PAD_IDLE state */
    mock_time_ms = 0;
    while (ctx.current_state != PAD_IDLE && mock_time_ms < 30000) {
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
        mock_time_ms += 100;
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);

    /* Verify initial config */
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, ctx.config.pyro1_mode);

    /* Write new config to filesystem */
    config_t new_cfg;
    config_set_defaults(&new_cfg);
    strncpy(new_cfg.id, "RELOAD1", 8);
    new_cfg.id[8] = '\0';
    new_cfg.pyro1_mode = PYRO_MODE_AGL;
    new_cfg.pyro1_value = 500;
    hal_config_save(&new_cfg);

    /* Reload config (should succeed in PAD_IDLE) */
    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(0, reload_result);

    /* Verify config updated in running system */
    TEST_ASSERT_EQUAL_STRING("RELOAD1", ctx.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, ctx.config.pyro1_mode);
    TEST_ASSERT_EQUAL(500, ctx.config.pyro1_value);
}

/* ── Test 3: Runtime reload rejected during ASCENT ── */
void test_config_reload_rejected_during_ascent(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    /* Force state to ASCENT */
    ctx.current_state = ASCENT;

    /* Write new config to filesystem */
    config_t new_cfg;
    config_set_defaults(&new_cfg);
    strncpy(new_cfg.id, "HACKED!", 8);
    new_cfg.id[8] = '\0';
    hal_config_save(&new_cfg);

    /* Attempt reload (should fail in ASCENT) */
    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-1, reload_result);

    /* Verify old config unchanged */
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
}

/* ── Test 4: Runtime reload rejected during FALLING (descent) ── */
void test_config_reload_rejected_during_descent(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    ctx.current_state = FALLING;

    config_t new_cfg;
    config_set_defaults(&new_cfg);
    strncpy(new_cfg.id, "BADCFG", 7);
    new_cfg.id[7] = '\0';
    hal_config_save(&new_cfg);

    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-1, reload_result);
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
}

/* ── Test 5: Runtime reload rejected during LANDED ── */
void test_config_reload_rejected_during_landed(void) {
    flight_context_t ctx;
    flight_init(&ctx);
    ctx.current_state = LANDED;

    config_t new_cfg;
    config_set_defaults(&new_cfg);
    strncpy(new_cfg.id, "UNSAFE", 7);
    new_cfg.id[7] = '\0';
    hal_config_save(&new_cfg);

    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-1, reload_result);
    TEST_ASSERT_EQUAL_STRING("PYRO001", ctx.config.id);
}

/* ── Test 6: Invalid config rejected on reload ── */
void test_config_reload_rejects_invalid_pyro_mode(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    /* Advance to PAD_IDLE */
    mock_time_ms = 0;
    while (ctx.current_state != PAD_IDLE && mock_time_ms < 30000) {
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
        mock_time_ms += 100;
    }

    /* Write config with invalid pyro mode */
    config_t bad_cfg;
    config_set_defaults(&bad_cfg);
    bad_cfg.pyro1_mode = 99; /* Invalid mode */
    hal_config_save(&bad_cfg);

    /* Reload should fail validation */
    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-3, reload_result); /* Validation error */

    /* Old config unchanged */
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, ctx.config.pyro1_mode);
}

/* ── Test 7: Config reload fails if file missing ── */
void test_config_reload_fails_if_file_missing(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    /* Advance to PAD_IDLE */
    mock_time_ms = 0;
    while (ctx.current_state != PAD_IDLE && mock_time_ms < 30000) {
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
        mock_time_ms += 100;
    }

    /* Clear filesystem (delete config.ini) */
    mock_reset_all();

    /* Reload should fail */
    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-2, reload_result); /* Load failed */
}

/* ── Test 8: Multiple config changes persist correctly ── */
void test_multiple_config_changes_persist(void) {
    /* Change 1 */
    config_t cfg1;
    config_set_defaults(&cfg1);
    cfg1.pyro1_value = 100;
    hal_config_save(&cfg1);

    /* Change 2 */
    config_t cfg2;
    config_set_defaults(&cfg2);
    hal_config_load(&cfg2);
    cfg2.pyro1_value = 200;
    cfg2.pyro2_value = 300;
    hal_config_save(&cfg2);

    /* Change 3 */
    config_t cfg3;
    config_set_defaults(&cfg3);
    hal_config_load(&cfg3);
    cfg3.pyro1_value = 400;
    hal_config_save(&cfg3);

    /* Final load should have latest values */
    config_t final;
    hal_config_load(&final);
    TEST_ASSERT_EQUAL(400, final.pyro1_value);
    TEST_ASSERT_EQUAL(300, final.pyro2_value); /* from change 2 */
}

/* ── Test 9: Config survives flight cycle ── */
void test_config_survives_flight_cycle(void) {
    /* Set custom config before flight */
    config_t pre_flight;
    config_set_defaults(&pre_flight);
    strncpy(pre_flight.id, "FLIGHT1", 8);
    pre_flight.id[8] = '\0';
    pre_flight.pyro1_mode = PYRO_MODE_AGL;
    pre_flight.pyro1_value = 300;
    hal_config_save(&pre_flight);

    /* Simulate flight */
    flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_EQUAL_STRING("FLIGHT1", ctx.config.id);

    /* After flight, simulate new boot */
    mock_reset_all();
    /* Re-save config since mock_reset_all() clears filesystem */
    hal_config_save(&pre_flight);

    flight_context_t ctx2;
    flight_init(&ctx2);

    /* Config should still be custom values */
    TEST_ASSERT_EQUAL_STRING("FLIGHT1", ctx2.config.id);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, ctx2.config.pyro1_mode);
    TEST_ASSERT_EQUAL(300, ctx2.config.pyro1_value);
}

/* ── Test 10: Invalid units rejected on reload ── */
void test_config_reload_rejects_invalid_units(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    /* Advance to PAD_IDLE */
    mock_time_ms = 0;
    while (ctx.current_state != PAD_IDLE && mock_time_ms < 30000) {
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
        mock_time_ms += 100;
    }

    /* Write config with invalid units */
    config_t bad_cfg;
    config_set_defaults(&bad_cfg);
    bad_cfg.units = 5; /* Invalid - only 0, 1, 2 are valid */
    hal_config_save(&bad_cfg);

    /* Reload should fail validation */
    int reload_result = flight_config_reload(&ctx);
    TEST_ASSERT_EQUAL(-3, reload_result); /* Validation error */

    /* Old config unchanged */
    TEST_ASSERT_EQUAL(1, ctx.config.units); /* default is meters */
}

/* ── Test 11: flight_get_context returns correct pointer ── */
void test_flight_get_context_returns_correct_pointer(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    flight_context_t *retrieved = flight_get_context();
    TEST_ASSERT_EQUAL_PTR(&ctx, retrieved);
}

/* ── Test 12: flight_get_state returns correct state ── */
void test_flight_get_state_returns_correct_state(void) {
    flight_context_t ctx;
    flight_init(&ctx);

    /* Should start in BOOT_SETTLE */
    TEST_ASSERT_EQUAL(BOOT_SETTLE, flight_get_state());

    /* Advance to PAD_IDLE */
    mock_time_ms = 0;
    while (ctx.current_state != PAD_IDLE && mock_time_ms < 30000) {
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
        mock_time_ms += 100;
    }

    TEST_ASSERT_EQUAL(PAD_IDLE, flight_get_state());
}

/* ── Test Runner ── */
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_config_persists_across_power_cycle);
    RUN_TEST(test_config_reload_succeeds_in_pad_idle);
    RUN_TEST(test_config_reload_rejected_during_ascent);
    RUN_TEST(test_config_reload_rejected_during_descent);
    RUN_TEST(test_config_reload_rejected_during_landed);
    RUN_TEST(test_config_reload_rejects_invalid_pyro_mode);
    RUN_TEST(test_config_reload_fails_if_file_missing);
    RUN_TEST(test_multiple_config_changes_persist);
    RUN_TEST(test_config_survives_flight_cycle);
    RUN_TEST(test_config_reload_rejects_invalid_units);
    RUN_TEST(test_flight_get_context_returns_correct_pointer);
    RUN_TEST(test_flight_get_state_returns_correct_state);

    return UNITY_END();
}