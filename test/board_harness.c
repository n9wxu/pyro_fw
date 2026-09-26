/*
 * See board_harness.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_harness.h"
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "../src/brownout.h"
#include "../src/buzzer.h"
#include "../src/hal.h"
#include "pressure_processing.h"

/* The announcements are not under test here. */
void buzzer_init(void) {}
void buzzer_play_spec(const beep_spec_t *spec, uint16_t gap_ms, uint8_t repeat_count) {
    (void)spec;
    (void)gap_ms;
    (void)repeat_count;
}
void buzzer_play_altitude(int32_t altitude) {
    (void)altitude;
}
void buzzer_play_usb_ok(void) {}
void buzzer_stop(void) {}
bool buzzer_is_active(void) {
    return false;
}

flight_context_t ctx;
board_power_t power;
uint32_t (*loop_lag_ms)(uint32_t t);

void boot_like_hardware(uint32_t seed) {
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    memset(&power, 0, sizeof(power));
    power.landing_timeout = -1;
    loop_lag_ms = NULL;
    mock_pressure.pressure_pa = 101325.0f;
    mock_noise_rms_pa = SENSOR_RMS_PA;
    mock_noise_seed = seed;
    mock_stall_seed = seed * 7919u + 1u;
}

static void power_on(void) {
    flight_init(&ctx);
    hal_pyro_claim_channels(mock_pyro_pads);
    if (power.landing_timeout >= 0)
        ctx.config.landing_timeout = (uint8_t)power.landing_timeout;
    ctx.usb_attached = power.usb;
    power.powered = true;
}

void tick(uint32_t t) {
    if (!power.powered)
        power_on();
    mock_time_ms = t;
    mock_pyro.firing = false;
    hal_tasks_tick(t);
    if (mock_core0_stalled(t))
        return;
    uint32_t now = t + (loop_lag_ms ? loop_lag_ms(t) : 0u);
    ctx.current_state = dispatch_state(&ctx, now);
    flight_update_outputs(&ctx, now);
    flight_flash_service(&ctx, now);
}

uint32_t run_to_pad(uint32_t *t) {
    for (; *t < 20000; (*t)++) {
        tick(*t);
        if (ctx.current_state == PAD_IDLE)
            return *t;
    }
    TEST_FAIL_MESSAGE("never reached PAD_IDLE");
    return 0;
}

bool booting(void) {
    return ctx.current_state == BOOT_SETTLE || ctx.current_state == BOOT_SENSOR;
}

void write_marker(int32_t ground_pa) {
    pad_marker_t m;
    pad_marker_fill(&m, ground_pa, (uint32_t)(PP_SIGMA_FLOOR_PA * 1000.0f));
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));
}

bool marker_valid(void) {
    pad_marker_t m;
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    return n == (int)sizeof(m) && pad_marker_valid(&m);
}
