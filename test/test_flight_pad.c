/*
 * On the pad: the announcement and the pyro health behind it, the ground
 * reference, the launch, the pad record, and a board on USB
 * [BUZ-01, BUZ-CODE-02, PYR-CONT-01..03, PYR-HEALTH-01/02, GND-CAL-01..07,
 * FLT-LAUNCH-02..07, FLT-BROWN-01, USB-01..08, CFG-10].
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board_harness.h"
#include "flight_run.h"
#include "../src/atmosphere.h"
#include "../src/pad_check.h"
#include "../src/pressure_processing.h"

void setUp(void) {}
void tearDown(void) {}

#define PAD_PA 101325.0f

static uint32_t now;

static void to_the_pad(uint32_t seed) {
    boot_like_hardware(seed);
    now = 0;
}

static void wait_ms(uint32_t ms) {
    for (uint32_t end = now + ms; now < end; now++)
        tick(now);
}

static void reach_pad(void) {
    run_to_pad(&now);
    wait_ms(1500); /* past the first health check */
}

static void fault(uint8_t channel, bool faulted) {
    if (channel == 1) {
        mock_pyro.p1_good = !faulted;
        mock_pyro.p1_open = faulted;
    } else {
        mock_pyro.p2_good = !faulted;
        mock_pyro.p2_open = faulted;
    }
}

/* A climb at a steady acceleration from the pad, for ms. */
static void climb(float accel_ms2, uint32_t ms) {
    uint32_t start = now;
    for (uint32_t end = now + ms; now < end; now++) {
        float t = (float)(now - start) / 1000.0f;
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, 0.5f * accel_ms2 * t * t);
        tick(now);
    }
}

/* ── The announcement [BUZ-01, BUZ-CODE-02, PYR-CONT-01..03] ──────── */

void test_BUZ_01_a_clean_board_says_ok_to_fly(void) {
    to_the_pad(1);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
    TEST_ASSERT_EQUAL_UINT16(0, ctx.diag);
}

void test_BUZ_01_a_pyro_fault_names_its_channel(void) {
    to_the_pad(2);
    fault(2, true);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_CHECK_PYRO_2));
    to_the_pad(2);
    fault(1, true);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_CHECK_PYRO_1));
}

/* One at a time: with both faulted the pad says pyro 1; once that is fixed
 * it says pyro 2, with no power cycle. Both are on the status report. */
void test_BUZ_CODE_02_pyro_1_is_announced_before_pyro_2(void) {
    to_the_pad(3);
    fault(1, true);
    fault(2, true);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_CHECK_PYRO_1));
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_PYRO_1) && (ctx.diag & DIAG_PYRO_2), "both are diagnosed [FLT-BOOT-15]");
    fault(1, false);
    wait_ms(1500);
    TEST_ASSERT_TRUE_MESSAGE(harness_last_said(BR_CHECK_PYRO_2), "pyro 2 is revealed once pyro 1 is fixed");
    fault(2, false);
    wait_ms(1500);
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
}

void test_BUZ_CODE_02_a_general_fault_outranks_a_pyro_fault(void) {
    TEST_ASSERT_EQUAL(BR_GENERAL_FAULT, pad_check_announcement(DIAG_SENSOR_FAIL | DIAG_P1_OPEN | DIAG_P2_SHORT));
    TEST_ASSERT_EQUAL(BR_GENERAL_FAULT, pad_check_announcement(DIAG_FS_FAIL));
    TEST_ASSERT_EQUAL(BR_CHECK_PYRO_1, pad_check_announcement(DIAG_P1_SHORT | DIAG_P2_OPEN));
    TEST_ASSERT_EQUAL(BR_CHECK_PYRO_2, pad_check_announcement(DIAG_P2_OPEN));
    TEST_ASSERT_EQUAL(BR_OK_TO_FLY, pad_check_announcement(0));
    /* A flight that was resumed is not a fault. */
    TEST_ASSERT_EQUAL(BR_OK_TO_FLY, pad_check_announcement(DIAG_RESUMED));
    /* [SNS-PRES-17] A sensor that stuck or stopped is one nobody can correct at the rocket. */
    TEST_ASSERT_EQUAL(BR_GENERAL_FAULT, pad_check_announcement(DIAG_SENSOR_STUCK | DIAG_P1_OPEN));
    TEST_ASSERT_EQUAL(BR_GENERAL_FAULT, pad_check_announcement(DIAG_SENSOR_LOST));
}

/* [SNS-PRES-17] The sensor stops answering on the pad: after half a second
 * the board says general fault and names it, and goes on saying so when the
 * sensor comes back, until it is restarted. */
void test_SNS_PRES_17_a_sensor_that_stops_on_the_pad_is_a_general_fault(void) {
    to_the_pad(36);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
    mock_pressure.sensor_type = 0;
    wait_ms(400);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, ctx.diag & DIAG_SENSOR_LOST, "a gap shorter than half a second is not a lost sensor");
    wait_ms(1500);
    TEST_ASSERT_TRUE((ctx.diag & DIAG_SENSOR_LOST) != 0);
    TEST_ASSERT_TRUE(harness_last_said(BR_GENERAL_FAULT));
    mock_pressure.sensor_type = 2;
    wait_ms(3000);
    TEST_ASSERT_TRUE_MESSAGE(harness_last_said(BR_GENERAL_FAULT), "a sensor that came back is still one that stopped");
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

/* [SNS-PRES-17] So is one that repeats itself for a second. */
void test_SNS_PRES_17_a_stuck_sensor_on_the_pad_is_a_general_fault(void) {
    to_the_pad(37);
    reach_pad();
    mock_sensor_stuck = true;
    wait_ms(2500);
    TEST_ASSERT_TRUE((ctx.diag & DIAG_SENSOR_STUCK) != 0);
    TEST_ASSERT_TRUE(harness_last_said(BR_GENERAL_FAULT));
}

/* A board newly on the pad has had no sample yet: that is not a lost sensor. */
void test_SNS_PRES_17_a_working_sensor_is_no_fault(void) {
    to_the_pad(38);
    reach_pad();
    wait_ms(60000);
    TEST_ASSERT_EQUAL_UINT16(0, ctx.diag & (DIAG_SENSOR_LOST | DIAG_SENSOR_STUCK));
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
}

void test_PYR_CONT_03_a_fault_that_appears_on_the_pad_is_announced(void) {
    to_the_pad(4);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
    fault(1, true); /* a lead lets go during the wait */
    wait_ms(1500);
    TEST_ASSERT_TRUE(harness_last_said(BR_CHECK_PYRO_1));
}

void test_PYR_CONT_01_health_is_checked_every_second(void) {
    to_the_pad(5);
    reach_pad();
    int before = mock_pyro.sample_count;
    wait_ms(10000);
    TEST_ASSERT_INT_WITHIN(1, 10, mock_pyro.sample_count - before);
}

void test_PYR_CONT_01_no_second_passes_without_a_check(void) {
    to_the_pad(50);
    reach_pad();
    wait_ms(1500);
    for (int second = 0; second < 1100; second++) {
        int before = mock_pyro.sample_count;
        wait_ms(1000);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(1, mock_pyro.sample_count - before);
    }
}

/* PAD_IDLE would open launch detection again in flight. */
void test_FLT_PHASE_04_a_state_that_is_no_state_is_a_fault(void) {
    to_the_pad(51);
    reach_pad();
    ctx.current_state = (flight_state_t)(STATE_COUNT + 3);
    wait_ms(100);
    TEST_ASSERT_EQUAL(FAULT, ctx.current_state);
    TEST_ASSERT_TRUE(harness_last_said(BR_GENERAL_FAULT));
}

/* ── Only enabled channels count [PYR-HEALTH-02] ──────────────────── */

void test_PYR_HEALTH_02_a_channel_set_to_none_is_not_a_fault(void) {
    to_the_pad(6);
    harness_config("[pyro]\npyro2_mode=none\n");
    fault(2, true);
    reach_pad();
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
    TEST_ASSERT_EQUAL_UINT16(0, ctx.diag & DIAG_PYRO_ANY);
}

/* The pin assignment is the source of truth: pyro 1's pad is the script's,
 * so the pyro settings left for it are stale and ignored. */
void test_PYR_HEALTH_02_a_channel_given_to_the_script_is_not_a_pyro(void) {
    to_the_pad(7);
    harness_config("[pyro]\npyro1_mode=delay\npyro1_value=0\n");
    harness_give_to_script(1);
    reach_pad();
    TEST_ASSERT_TRUE_MESSAGE(harness_last_said(BR_OK_TO_FLY), "its open circuit is not a fault");
    TEST_ASSERT_FALSE(ctx.plan.enabled[0]);
    TEST_ASSERT_TRUE(ctx.plan.enabled[1]);
}

/* ── Configuration takes effect at start-up [CFG-10] ──────────────── */

void test_CFG_10_a_saved_change_waits_for_the_next_start(void) {
    to_the_pad(8);
    harness_config("[pyro]\nunits=m\npyro2_mode=agl\npyro2_value=300\n");
    reach_pad();
    harness_config("[pyro]\nunits=m\npyro2_mode=agl\npyro2_value=150\nfire_gap=5000\n");
    wait_ms(3000);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 300.0f, ctx.plan.trigger_m[1],
                                     "the running board keeps what it started with");
    TEST_ASSERT_EQUAL_UINT32(3000, ctx.plan.fire_gap_ms);
    harness_restart(RESET_SOFTWARE);
    reach_pad();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 150.0f, ctx.plan.trigger_m[1], "and takes the change at the next start");
    TEST_ASSERT_EQUAL_UINT32(5000, ctx.plan.fire_gap_ms);
}

/* ── Launch [FLT-LAUNCH-02..07] ───────────────────────────────────── */

void test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch(void) {
    to_the_pad(9);
    reach_pad();
    wait_ms(6000);
    climb(60.0f, 1500);
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    TEST_ASSERT_TRUE_MESSAGE(harness_stops >= 1, "the buzzer stops at launch [FLT-LAUNCH-05]");
    TEST_ASSERT_EQUAL_INT(1, harness_log_events("LAUNCH"));
}

void test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch(void) {
    to_the_pad(10);
    reach_pad();
    wait_ms(6000);
    /* Thrown 25 m up and caught: fast, but never 100 ft. */
    uint32_t start = now;
    for (; now < start + 4500u; now++) {
        float t = (float)(now - start) / 1000.0f;
        float h = 22.0f * t - 4.9f * t * t;
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, h > 0.0f ? h : 0.0f);
        tick(now);
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

/* Carried up a 60 m tower at 1 m/s: high enough, never fast enough. */
void test_FLT_LAUNCH_07_a_slow_rise_is_not_a_launch(void) {
    to_the_pad(11);
    reach_pad();
    wait_ms(6000);
    uint32_t start = now;
    for (; now < start + 60000u; now++) {
        mock_pressure.pressure_pa = atmos_pressure_above_pa(PAD_PA, (float)(now - start) / 1000.0f);
        tick(now);
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

/* ── Bad readings and gusts [SNS-EST-02, SNS-EST-03] ──────────────── */

void test_SNS_EST_02_one_or_two_bad_readings_are_not_a_launch(void) {
    const int32_t sizes[] = {-300, -3000, -12000, -60000, 3000, 20000};
    for (int run = 1; run <= 2; run++) {
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            to_the_pad(12 + k);
            reach_pad();
            wait_ms(6000);
            mock_glitch_pa = sizes[k];
            mock_glitch_samples = run;
            wait_ms(3000);
            char msg[64];
            snprintf(msg, sizeof(msg), "%d reading(s) %ld Pa off", run, (long)sizes[k]);
            TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, msg);
            TEST_ASSERT_INT32_WITHIN_MESSAGE(3, (int32_t)PAD_PA, pp_ground_pressure(), msg);
        }
    }
}

/* Half an hour of gusts at 30 Pa rms, on sensors from 1.2 to 5 Pa. */
void test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch(void) {
    const float noises[] = {SENSOR_RMS_PA, NOISY_SENSOR_RMS_PA};
    for (unsigned n = 0; n < 2; n++) {
        to_the_pad(20 + n);
        mock_noise_rms_pa = noises[n];
        reach_pad();
        uint32_t rng = 12345u + n;
        float gust = 0.0f;
        const float keep = expf(-0.001f / 3.0f), drive = 30.0f * sqrtf(1.0f - keep * keep);
        for (uint32_t end = now + 1800000u; now < end; now++) {
            rng = rng * 1664525u + 1013904223u;
            /* The sum of three uniforms, scaled to unit variance. */
            float u = ((float)(rng >> 8 & 0xFFFFu) + (float)(rng >> 16 & 0xFFu) * 256.0f) / 65536.0f - 1.0f;
            gust = keep * gust + drive * u * 1.7f;
            mock_pressure.pressure_pa = PAD_PA + gust;
            tick(now);
            if (ctx.current_state != PAD_IDLE)
                break;
        }
        TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "a launch declared in gusts");
    }
}

/* ── The ground reference [GND-CAL-01..07] ────────────────────────── */

/* [FLT-RATE-06] The collector is faster than the loop: three samples waiting
 * are three steps of this loop, and the newest is the one the flight
 * software stands on. Taken one a loop they would queue, and every decision
 * would be made on old data. */
void test_FLT_RATE_06_every_waiting_sample_is_taken_in_its_loop(void) {
    to_the_pad(35);
    reach_pad();
    while (pp_available())
        ctx.current_state = dispatch_state(&ctx, now);
    uint64_t stamp_us = (uint64_t)now * 1000u;
    for (int i = 0; i < 3; i++) {
        stamp_us += 18867u;
        pp_feed_us(pp_ground_pressure() + i, stamp_us);
    }
    TEST_ASSERT_EQUAL_INT(3, pp_available());
    ctx.current_state = dispatch_state(&ctx, now);
    TEST_ASSERT_EQUAL_INT(0, pp_available());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(stamp_us / 1000u), ctx.last_sample);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

void test_GND_CAL_01_the_reference_follows_the_weather(void) {
    to_the_pad(30);
    reach_pad();
    uint32_t start = now;
    for (; now < start + 60000u; now++) { /* 0.5 Pa a second for a minute */
        mock_pressure.pressure_pa = PAD_PA - 0.5f * (float)(now - start) / 1000.0f;
        tick(now);
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_INT32_WITHIN(4, (int32_t)(PAD_PA - 30.0f), pp_ground_pressure());
}

/* [GND-CAL-03..05] A launch does not drag the reference up with it: frozen,
 * it is still the pad's, and the altitude at detection is the height
 * reached. */
void test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure(void) {
    to_the_pad(31);
    reach_pad();
    wait_ms(8000);
    climb(30.0f, 2000);
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    TEST_ASSERT_INT32_WITHIN(3, (int32_t)PAD_PA, ctx.ground_pressure);
    TEST_ASSERT_FALSE(pp_ground_degraded());
    TEST_ASSERT_TRUE_MESSAGE(ctx.altitude_cm > 3048, "the height reached, not zero");
}

/* [GND-CAL-06] The board carried to a pad 40 m lower: every reading is now
 * outside the gate, the board is still, and after 5 s the reference moves. */
void test_GND_CAL_06_a_moved_board_takes_a_new_reference(void) {
    to_the_pad(32);
    reach_pad();
    wait_ms(6000);
    mock_pressure.pressure_pa = PAD_PA + 480.0f;
    wait_ms(3000);
    TEST_ASSERT_INT32_WITHIN_MESSAGE(3, (int32_t)PAD_PA, pp_ground_pressure(), "not within the first seconds");
    wait_ms(5000);
    TEST_ASSERT_INT32_WITHIN(4, (int32_t)(PAD_PA + 480.0f), pp_ground_pressure());
    TEST_ASSERT_EQUAL_UINT32(1, pp_ground_reseeds());
    TEST_ASSERT_NOT_NULL(strstr(mock_uart_buf, "!GND reseed"));
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

void test_GND_CAL_06_a_launch_never_moves_the_reference(void) {
    to_the_pad(33);
    reach_pad();
    wait_ms(6000);
    climb(15.0f, 9000);
    TEST_ASSERT_EQUAL_UINT32(0, pp_ground_reseeds());
    TEST_ASSERT_INT32_WITHIN(3, (int32_t)PAD_PA, ctx.ground_pressure);
}

/* [GND-CAL-07] Launched within a second of reaching the pad: the reference
 * rests on too little of it, and says so. */
void test_GND_CAL_07_a_reference_from_under_a_second_is_reported(void) {
    to_the_pad(34);
    run_to_pad(&now);
    wait_ms(400);
    climb(60.0f, 1500);
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    TEST_ASSERT_TRUE(pp_ground_degraded());
}

/* ── The pad record [FLT-BROWN-01] ────────────────────────────────── */

void test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds(void) {
    to_the_pad(40);
    run_to_pad(&now);
    wait_ms(9000);
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "not before the dwell");
    wait_ms(2000);
    TEST_ASSERT_TRUE(pad_record_stored());
    uint32_t writes = mock_fs_write_count;
    wait_ms(30000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, mock_fs_write_count, "and once only");
}

/* ── On USB [USB-01..08] ──────────────────────────────────────────── */

void test_USB_01_no_launch_and_no_record_while_attached(void) {
    to_the_pad(50);
    harness_usb(true);
    reach_pad();
    wait_ms(12000);
    TEST_ASSERT_FALSE_MESSAGE(pad_record_stored(), "the bench is not the pad");
    climb(60.0f, 2000);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

void test_USB_02_nothing_is_announced_while_attached(void) {
    to_the_pad(51);
    harness_usb(true);
    fault(1, true);
    reach_pad();
    wait_ms(5000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, harness_spec_plays, "the verdict is not said on USB");
    TEST_ASSERT_TRUE_MESSAGE(ctx.diag & DIAG_PYRO_1, "it is still diagnosed, for the status report");
}

void test_USB_03_one_chirp_on_attach_and_the_verdict_on_detach(void) {
    to_the_pad(52);
    reach_pad();
    int said = harness_spec_plays;
    harness_usb(true);
    wait_ms(3000);
    TEST_ASSERT_EQUAL_INT(1, harness_usb_ok_plays);
    TEST_ASSERT_EQUAL_INT(said, harness_spec_plays);
    harness_usb(false);
    wait_ms(1500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(said + 1, harness_spec_plays, "said again on detach [USB-04]");
    TEST_ASSERT_TRUE(harness_last_said(BR_OK_TO_FLY));
}

/* [USB-04] The dwell of the pad record starts again on detach. */
void test_USB_04_the_record_waits_ten_seconds_from_the_detach(void) {
    to_the_pad(53);
    harness_usb(true);
    reach_pad();
    wait_ms(20000);
    harness_usb(false);
    wait_ms(8000);
    TEST_ASSERT_FALSE(pad_record_stored());
    wait_ms(3000);
    TEST_ASSERT_TRUE(pad_record_stored());
}

void test_USB_05_attachment_is_ignored_in_flight(void) {
    to_the_pad(54);
    reach_pad();
    wait_ms(6000);
    climb(60.0f, 1500);
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    harness_usb(true);
    wait_ms(100);
    TEST_ASSERT_FALSE_MESSAGE(ctx.usb_attached, "a flight is never abandoned on the strength of it");
    TEST_ASSERT_EQUAL_INT(0, harness_usb_ok_plays);
}

/* [USB-08] Test mode makes a board on USB behave as on battery; it is off
 * at every start. */
void test_USB_08_test_mode_flies_on_usb(void) {
    to_the_pad(55);
    harness_usb(true);
    reach_pad();
    TEST_ASSERT_FALSE_MESSAGE(ctx.test_mode, "off at start-up");
    flight_set_test_mode(&ctx, true, now);
    wait_ms(12000);
    TEST_ASSERT_TRUE_MESSAGE(harness_last_said(BR_OK_TO_FLY), "the verdict is announced");
    TEST_ASSERT_TRUE_MESSAGE(pad_record_stored(), "the pad is recorded");
    climb(60.0f, 1500);
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    flight_set_test_mode(&ctx, false, now);
    TEST_ASSERT_TRUE_MESSAGE(ctx.test_mode, "and it does not change in flight");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_BUZ_01_a_clean_board_says_ok_to_fly);
    RUN_TEST(test_BUZ_01_a_pyro_fault_names_its_channel);
    RUN_TEST(test_BUZ_CODE_02_pyro_1_is_announced_before_pyro_2);
    RUN_TEST(test_BUZ_CODE_02_a_general_fault_outranks_a_pyro_fault);
    RUN_TEST(test_PYR_CONT_03_a_fault_that_appears_on_the_pad_is_announced);
    RUN_TEST(test_PYR_CONT_01_health_is_checked_every_second);
    RUN_TEST(test_PYR_CONT_01_no_second_passes_without_a_check);
    RUN_TEST(test_FLT_PHASE_04_a_state_that_is_no_state_is_a_fault);
    RUN_TEST(test_PYR_HEALTH_02_a_channel_set_to_none_is_not_a_fault);
    RUN_TEST(test_PYR_HEALTH_02_a_channel_given_to_the_script_is_not_a_pyro);
    RUN_TEST(test_CFG_10_a_saved_change_waits_for_the_next_start);
    RUN_TEST(test_FLT_LAUNCH_07_a_climb_past_100_ft_is_a_launch);
    RUN_TEST(test_FLT_LAUNCH_02_below_100_ft_is_not_a_launch);
    RUN_TEST(test_FLT_LAUNCH_07_a_slow_rise_is_not_a_launch);
    RUN_TEST(test_SNS_EST_02_one_or_two_bad_readings_are_not_a_launch);
    RUN_TEST(test_SNS_EST_03_gusts_on_the_pad_are_not_a_launch);
    RUN_TEST(test_GND_CAL_01_the_reference_follows_the_weather);
    RUN_TEST(test_GND_CAL_04_the_reference_freezes_at_the_pads_pressure);
    RUN_TEST(test_GND_CAL_06_a_moved_board_takes_a_new_reference);
    RUN_TEST(test_GND_CAL_06_a_launch_never_moves_the_reference);
    RUN_TEST(test_GND_CAL_07_a_reference_from_under_a_second_is_reported);
    RUN_TEST(test_FLT_RATE_06_every_waiting_sample_is_taken_in_its_loop);
    RUN_TEST(test_SNS_PRES_17_a_sensor_that_stops_on_the_pad_is_a_general_fault);
    RUN_TEST(test_SNS_PRES_17_a_stuck_sensor_on_the_pad_is_a_general_fault);
    RUN_TEST(test_SNS_PRES_17_a_working_sensor_is_no_fault);
    RUN_TEST(test_FLT_BROWN_01_the_pad_is_recorded_after_ten_seconds);
    RUN_TEST(test_USB_01_no_launch_and_no_record_while_attached);
    RUN_TEST(test_USB_02_nothing_is_announced_while_attached);
    RUN_TEST(test_USB_03_one_chirp_on_attach_and_the_verdict_on_detach);
    RUN_TEST(test_USB_04_the_record_waits_ten_seconds_from_the_detach);
    RUN_TEST(test_USB_05_attachment_is_ignored_in_flight);
    RUN_TEST(test_USB_08_test_mode_flies_on_usb);
    return UNITY_END();
}
