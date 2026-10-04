/*
 * Start-up: the checks before the pad, and the terminal fault when one of
 * them cannot pass [FLT-BOOT-01..08, FLT-BOOT-12..14, TEL-05, USB-02].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "board_harness.h"
#include "board_pins.h"
#include "../src/pad_check.h"
#include "../src/pressure_processing.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t now;

static void power_up(uint32_t seed) {
    boot_like_hardware(seed);
    now = 0;
}

static void wait_ms(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now++)
        tick(now);
}

static void run_until_state(flight_state_t state, uint32_t limit_ms) {
    for (uint32_t end = now + limit_ms; now < end; now++) {
        tick(now);
        if (ctx.current_state == state)
            return;
    }
    TEST_FAIL_MESSAGE("the state was never reached");
}

static int lines_with(const char *text) {
    int n = 0;
    for (const char *p = mock_uart_buf; (p = strstr(p, text)) != NULL; p += strlen(text))
        n++;
    return n;
}

/* ── The checks, in order [FLT-BOOT-01..08] ───────────────────────── */

void test_FLT_BOOT_01_every_check_runs_before_the_pad(void) {
    power_up(1);
    bool seen[STATE_COUNT] = {false};
    for (; now < 20000u && ctx.current_state != PAD_IDLE; now++) {
        tick(now);
        seen[ctx.current_state] = true;
        if (ctx.current_state != PAD_IDLE)
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, lines_with("$PYRO"), "no telemetry during start-up [TEL-05]");
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_TRUE(seen[BOOT_SETTLE] && seen[BOOT_SENSOR] && seen[BOOT_CALIBRATE]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, ctx.sensor_type, "the sensor was identified [FLT-BOOT-05]");
    TEST_ASSERT_TRUE_MESSAGE(mock_pyro.sample_count >= 1, "pyro health was read [FLT-BOOT-06, FLT-BOOT-07]");
    TEST_ASSERT_TRUE(ctx.channel_ready[0] && ctx.channel_ready[1]);
    TEST_ASSERT_INT32_WITHIN(3, 101325, ctx.ground_pressure);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_pulse_count, "and nothing is energised");
}

void test_FLT_BOOT_02_the_stored_configuration_is_what_flies(void) {
    power_up(2);
    harness_config("[pyro]\nunits=m\npyro1_mode=delay\npyro1_value=3\npyro2_mode=agl\npyro2_value=250\n"
                   "pyro2_refire_speed=30\nrefire_interval=2000\n");
    run_to_pad(&now);
    TEST_ASSERT_TRUE(ctx.plan.enabled[0] && ctx.plan.enabled[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, ctx.plan.trigger_m[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, ctx.plan.refire_ms[1]);
    TEST_ASSERT_EQUAL_UINT32(2000, ctx.plan.refire_interval_ms);
}

void test_FLT_BOOT_05_a_sensor_slow_to_answer_is_waited_for(void) {
    power_up(3);
    mock_pressure.pending_until_ms = 4000;
    run_to_pad(&now);
    TEST_ASSERT_TRUE(now > 4000u);
    TEST_ASSERT_EQUAL_UINT16(0, ctx.diag);
}

void test_FLT_BOOT_07_a_fault_found_at_start_up_is_the_first_thing_said(void) {
    power_up(4);
    mock_pyro.p2_good = false;
    mock_pyro.p2_open = true;
    run_to_pad(&now);
    wait_ms(1500);
    TEST_ASSERT_TRUE(harness_last_said(BR_CHECK_PYRO_2));
    TEST_ASSERT_FALSE(ctx.channel_ready[1]);
}

/* [FLT-BOOT-08] One reading 30 hPa off, in the middle of the calibration,
 * does not move the reference. */
void test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference(void) {
    const int32_t off[] = {-3000, 3000, -40000};
    for (unsigned k = 0; k < 3; k++) {
        power_up(5 + k);
        run_until_state(BOOT_CALIBRATE, 20000);
        uint32_t began = now;
        wait_ms(90);
        mock_glitch_pa = off[k];
        mock_glitch_samples = 1;
        run_to_pad(&now);
        TEST_ASSERT_TRUE_MESSAGE(now - began >= 9u * mock_sample_interval_ms, "from at least ten readings");
        TEST_ASSERT_INT32_WITHIN(3, 101325, pp_ground_pressure());
    }
}

/* ── The terminal fault [FLT-BOOT-12..14, TEL-05] ─────────────────── */

static void assert_terminal_general_fault(uint16_t diag_bit) {
    TEST_ASSERT_EQUAL(FAULT, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.diag & diag_bit);
    TEST_ASSERT_TRUE_MESSAGE(harness_last_said(BR_GENERAL_FAULT), "general fault is announced");
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    /* It stays there, whatever the barometer then shows. */
    mock_pressure.sensor_type = 2;
    mock_pressure.pressure_pa = 60000.0f;
    wait_ms(30000);
    TEST_ASSERT_EQUAL(FAULT, ctx.current_state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_pulse_count, "a faulted board fires nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lines_with("$PYRO"), "no telemetry in FAULT [TEL-05]");
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, 6, lines_with("!FAULT"), "a !FAULT line every 5 s [TEL-05]");
}

void test_FLT_BOOT_12_no_sensor_is_a_terminal_general_fault(void) {
    power_up(10);
    mock_pressure.sensor_type = 0;
    run_until_state(FAULT, 20000);
    assert_terminal_general_fault(DIAG_SENSOR_FAIL);
}

void test_FLT_BOOT_12_a_sensor_that_never_finishes_coming_up_is_a_fault(void) {
    power_up(11);
    mock_pressure.pending_until_ms = 600000;
    run_until_state(FAULT, 20000);
    TEST_ASSERT_TRUE_MESSAGE(now < 10000u, "the start does not hang on it");
    assert_terminal_general_fault(DIAG_SENSOR_FAIL);
}

void test_FLT_BOOT_13_a_calibration_with_no_samples_is_a_fault(void) {
    power_up(12);
    run_until_state(BOOT_CALIBRATE, 20000);
    uint32_t began = now;
    mock_pressure.sensor_type = 0; /* it answered, and now delivers nothing */
    run_until_state(FAULT, 20000);
    TEST_ASSERT_UINT32_WITHIN(200, 10000, now - began);
    assert_terminal_general_fault(DIAG_SENSOR_FAIL);
}

void test_FLT_BOOT_14_unusable_storage_is_a_terminal_general_fault(void) {
    power_up(13);
    mock_fs_unusable = true;
    run_until_state(FAULT, 20000);
    assert_terminal_general_fault(DIAG_FS_FAIL);
}

void test_FLT_BOOT_18_an_unreadable_configuration_is_a_fault_and_the_file_is_kept(void) {
    power_up(15);
    const char *ini = "[pyro]\npyro2_mode=agl\npyro2_value=123\n";
    harness_config(ini);
    mock_config_unreadable = true;
    run_until_state(FAULT, 20000);
    char kept[128];
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(ini), mock_fs_peek("config.ini", kept, (int)sizeof(kept)),
                                  "the operator's file is not replaced with defaults");
    TEST_ASSERT_EQUAL_STRING("config_unreadable", pad_check_fault_name(DIAG_CONFIG_UNREADABLE));
    assert_terminal_general_fault(DIAG_CONFIG_UNREADABLE);
}

/* The stamp of the image that ran here before is another board's. */
void test_FLT_BOOT_17_an_image_built_for_another_board_is_a_terminal_general_fault(void) {
    power_up(16);
    harness_board_stamp("another");
    run_until_state(FAULT, 20000);
    TEST_ASSERT_EQUAL_STRING("board_mismatch", pad_check_fault_name(DIAG_BOARD_MISMATCH));
    assert_terminal_general_fault(DIAG_BOARD_MISMATCH);
}

void test_FLT_BOOT_17_a_board_with_its_own_stamp_or_none_reaches_the_pad(void) {
    power_up(17);
    run_to_pad(&now);
    TEST_ASSERT_FALSE(ctx.diag & DIAG_BOARD_MISMATCH);
    power_up(18);
    harness_board_stamp(BOARD_SHORT_STR);
    run_to_pad(&now);
    TEST_ASSERT_FALSE(ctx.diag & DIAG_BOARD_MISMATCH);
}

/* [USB-02] On the bench the fault is diagnosed and not said. */
void test_USB_02_a_general_fault_is_not_said_on_usb(void) {
    power_up(14);
    harness_usb(true);
    mock_pressure.sensor_type = 0;
    run_until_state(FAULT, 20000);
    wait_ms(10000);
    TEST_ASSERT_EQUAL_INT(0, harness_spec_plays);
    TEST_ASSERT_TRUE(ctx.diag & DIAG_SENSOR_FAIL);
    TEST_ASSERT_EQUAL_UINT8(BR_GENERAL_FAULT, ctx.announcement);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLT_BOOT_01_every_check_runs_before_the_pad);
    RUN_TEST(test_FLT_BOOT_02_the_stored_configuration_is_what_flies);
    RUN_TEST(test_FLT_BOOT_05_a_sensor_slow_to_answer_is_waited_for);
    RUN_TEST(test_FLT_BOOT_07_a_fault_found_at_start_up_is_the_first_thing_said);
    RUN_TEST(test_FLT_BOOT_08_one_bad_reading_does_not_bias_the_reference);
    RUN_TEST(test_FLT_BOOT_12_no_sensor_is_a_terminal_general_fault);
    RUN_TEST(test_FLT_BOOT_12_a_sensor_that_never_finishes_coming_up_is_a_fault);
    RUN_TEST(test_FLT_BOOT_13_a_calibration_with_no_samples_is_a_fault);
    RUN_TEST(test_FLT_BOOT_14_unusable_storage_is_a_terminal_general_fault);
    RUN_TEST(test_FLT_BOOT_18_an_unreadable_configuration_is_a_fault_and_the_file_is_kept);
    RUN_TEST(test_FLT_BOOT_17_an_image_built_for_another_board_is_a_terminal_general_fault);
    RUN_TEST(test_FLT_BOOT_17_a_board_with_its_own_stamp_or_none_reaches_the_pad);
    RUN_TEST(test_USB_02_a_general_fault_is_not_said_on_usb);
    return UNITY_END();
}
