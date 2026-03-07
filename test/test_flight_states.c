/*
 * Unit tests for flight state machine and telemetry.
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "../src/flight_states.h"

/* Helper: advance boot to PAD_IDLE */
static void boot_to_pad_idle(flight_context_t *ctx) {
    ctx->current_state = BOOT_FILESYSTEM;
    /* Run through boot states */
    mock_time_ms = 0;
    ctx->current_state = dispatch_state(ctx, mock_time_ms);  /* FILESYSTEM → I2C_SETTLE */
    TEST_ASSERT_EQUAL(BOOT_I2C_SETTLE, ctx->current_state);

    mock_time_ms = 600;  /* past 500ms settle */
    ctx->current_state = dispatch_state(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(BOOT_SENSOR_DETECT, ctx->current_state);

    ctx->current_state = dispatch_state(ctx, mock_time_ms);  /* detect → PYRO_INIT */
    TEST_ASSERT_EQUAL(BOOT_PYRO_INIT, ctx->current_state);

    ctx->current_state = dispatch_state(ctx, mock_time_ms);  /* → CONTINUITY */
    TEST_ASSERT_EQUAL(BOOT_CONTINUITY, ctx->current_state);

    ctx->current_state = dispatch_state(ctx, mock_time_ms);  /* → STABILIZE */
    TEST_ASSERT_EQUAL(BOOT_STABILIZE, ctx->current_state);

    mock_time_ms = 2700;  /* past 2000ms stabilize */
    ctx->current_state = dispatch_state(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(BOOT_CALIBRATE, ctx->current_state);

    /* Run 10 calibration readings */
    for (int i = 0; i < 10; i++) {
        mock_time_ms += 110;
        ctx->current_state = dispatch_state(ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(BOOT_MDNS, ctx->current_state);

    ctx->current_state = dispatch_state(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx->current_state);
}

void setUp(void) {
    mock_reset_all();
}

void tearDown(void) {}

/* ── Helper tests ─────────────────────────────────────────────────── */

void test_pressure_to_altitude_cm(void) {
    /* Sea level: 0 altitude */
    TEST_ASSERT_EQUAL(0, pressure_to_altitude_cm(101325, 101325));
    /* Lower pressure = higher altitude */
    int32_t alt = pressure_to_altitude_cm(100325, 101325);
    TEST_ASSERT_TRUE(alt > 0);
    TEST_ASSERT_INT_WITHIN(1000, 8300, alt);  /* ~8.3 cm/Pa * 1000 Pa ≈ 8300 cm */
}

void test_filter_pressure_init(void) {
    flight_context_t ctx = {0};
    int32_t result = filter_pressure(&ctx, 101325, 10);
    TEST_ASSERT_EQUAL(101325, result);
    TEST_ASSERT_TRUE(ctx.filter_initialized);
}

void test_filter_pressure_smoothing(void) {
    flight_context_t ctx = {0};
    filter_pressure(&ctx, 101325, 10);
    /* Step change — filter should smooth */
    int32_t result = filter_pressure(&ctx, 100325, 100);
    TEST_ASSERT_TRUE(result < 101325);
    TEST_ASSERT_TRUE(result > 100325);
}

void test_buf_add(void) {
    flight_context_t ctx = {0};
    buf_add(&ctx, 100, 101325, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(1, ctx.buf_count);
    TEST_ASSERT_EQUAL(100, ctx.flight_buffer[0].time_ms);
    TEST_ASSERT_EQUAL(101325, ctx.flight_buffer[0].pressure_pa);
}

void test_buf_add_wraps(void) {
    flight_context_t ctx = {0};
    for (int i = 0; i < 4096; i++)
        buf_add(&ctx, i, 101325, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(4096, ctx.buf_count);
    /* One more should overwrite oldest */
    buf_add(&ctx, 9999, 101325, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(4096, ctx.buf_count);
}

/* ── Boot state tests ─────────────────────────────────────────────── */

void test_boot_sequence_reaches_pad_idle(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    boot_to_pad_idle(&ctx);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

void test_boot_calibrates_ground_pressure(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    mock_pressure.pressure_pa = 101000.0f;
    boot_to_pad_idle(&ctx);
    TEST_ASSERT_INT_WITHIN(100, 101000, ctx.ground_pressure);
}

void test_boot_no_sensor(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    mock_pressure.sensor_type = 0;
    boot_to_pad_idle(&ctx);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_EQUAL(0, ctx.ground_pressure);
}

void test_boot_i2c_settle_waits(void) {
    flight_context_t ctx = {0};
    ctx.current_state = BOOT_I2C_SETTLE;
    ctx.boot_timer = 0;
    /* Before 500ms — stays in settle */
    TEST_ASSERT_EQUAL(BOOT_I2C_SETTLE, dispatch_state(&ctx, 400));
    /* After 500ms — advances */
    TEST_ASSERT_EQUAL(BOOT_SENSOR_DETECT, dispatch_state(&ctx, 600));
}

/* ── PAD_IDLE tests ───────────────────────────────────────────────── */

void test_pad_idle_stays_on_ground(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    mock_pressure.pressure_pa = 101325.0f;

    mock_time_ms = 1000;
    ctx.last_sample = 0;
    flight_state_t next = dispatch_state(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(PAD_IDLE, next);
}

void test_pad_idle_detects_ascent(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = false;
    /* Simulate large altitude: pressure drop of ~150 Pa = ~1245 cm > 1000 cm threshold */
    mock_pressure.pressure_pa = 101325.0f - 150.0f;

    /* Run several samples so filter converges */
    for (int i = 0; i < 20; i++) {
        mock_time_ms = i * 15;
        ctx.last_sample = (i > 0) ? (i - 1) * 15 : 0;
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.launch_time > 0);
}

void test_pad_idle_continuity_check(void) {
    flight_context_t ctx = {0};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    mock_pressure.pressure_pa = 101325.0f;
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = false;
    mock_pyro.p2_open = true;
    mock_pyro.p1_adc = 42;
    mock_pyro.p2_adc = 4000;

    ctx.last_sample = 0;
    ctx.last_cont_check = 0;
    mock_time_ms = 1500;
    dispatch_state(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.pyro1_continuity_good);
    TEST_ASSERT_FALSE(ctx.pyro2_continuity_good);
    TEST_ASSERT_EQUAL(42, ctx.pyro1_adc);
    TEST_ASSERT_EQUAL(4000, ctx.pyro2_adc);
}

/* ── ASCENT tests ─────────────────────────────────────────────────── */

void test_ascent_tracks_max_altitude(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101000;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 2000;
    ctx.max_altitude = 2000;

    /* Higher altitude */
    mock_pressure.pressure_pa = 101325.0f - 400.0f;
    mock_time_ms = 200;
    dispatch_state(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.max_altitude >= 2000);
}

void test_ascent_arms_pyros_at_low_speed(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 100700;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 5000;
    ctx.vertical_speed_cms = 500;  /* 5 m/s — below 10 m/s threshold */

    /* Altitude slightly higher than last — positive but slow speed */
    mock_pressure.pressure_pa = 101325.0f - 620.0f;  /* ~5146 cm */
    mock_time_ms = 200;
    ctx.current_state = dispatch_state(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.pyros_armed);
}

void test_ascent_detects_apogee(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 100425;  /* close to what we'll read */
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 8000;
    ctx.max_altitude = 8000;
    ctx.pyros_armed = true;
    ctx.vertical_speed_cms = 100;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;

    /* Altitude lower than last → negative speed → apogee */
    mock_pressure.pressure_pa = 101325.0f - 900.0f;  /* ~7470 cm < 8000 */
    mock_time_ms = 200;
    ctx.current_state = dispatch_state(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(DESCENT, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.apogee_detected);
}

/* ── DESCENT tests ────────────────────────────────────────────────── */

void test_descent_detects_landing(void) {
    flight_context_t ctx = {0};
    ctx.current_state = DESCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101300;
    ctx.launch_time = 0;
    ctx.last_altitude = 100;
    ctx.apogee_detected = true;

    /* Stable altitude for >1 second */
    mock_pressure.pressure_pa = 101325.0f - 12.0f;  /* ~100 cm */
    ctx.last_sample = 0;
    mock_time_ms = 100;
    dispatch_state(&ctx, mock_time_ms);
    TEST_ASSERT_TRUE(ctx.landing_stable_since > 0);

    /* Keep stable */
    for (int i = 0; i < 20; i++) {
        mock_time_ms += 60;
        ctx.last_sample = mock_time_ms - 60;
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(LANDED, ctx.current_state);
}

/* ── LANDED tests ─────────────────────────────────────────────────── */

void test_landed_stays_landed(void) {
    flight_context_t ctx = {0};
    ctx.current_state = LANDED;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    mock_pressure.pressure_pa = 101325.0f;

    mock_time_ms = 2000;
    flight_state_t next = dispatch_state(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(LANDED, next);
}

/* ── Telemetry tests ──────────────────────────────────────────────── */

void test_telemetry_format(void) {
    flight_context_t ctx = {0};
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    ctx.pyro1_adc = 50;
    ctx.pyro2_adc = 55;
    ctx.filtered_pressure = 101000;
    ctx.vertical_speed_cms = 500;
    ctx.max_altitude = 10000;

    mock_uart_len = 0;
    send_telemetry(&ctx, 5000, 8000, PAD_IDLE);

    /* Should start with $PYRO and end with *XX\r\n */
    TEST_ASSERT_TRUE(mock_uart_len > 0);
    TEST_ASSERT_EQUAL('$', mock_uart_buf[0]);
    TEST_ASSERT_EQUAL('P', mock_uart_buf[1]);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "*") != NULL);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "\r\n") != NULL);
}

void test_telemetry_seq_increments(void) {
    flight_context_t ctx = {0};
    ctx.telemetry_seq = 0;

    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(1, ctx.telemetry_seq);

    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(2, ctx.telemetry_seq);
}

void test_telemetry_checksum(void) {
    flight_context_t ctx = {0};
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);

    /* Find $ and * */
    char *dollar = strchr(mock_uart_buf, '$');
    char *star = strchr(mock_uart_buf, '*');
    TEST_ASSERT_NOT_NULL(dollar);
    TEST_ASSERT_NOT_NULL(star);

    /* Compute expected checksum */
    uint8_t expected = 0;
    for (char *p = dollar + 1; p < star; p++)
        expected ^= (uint8_t)*p;

    /* Parse actual checksum */
    unsigned int actual;
    sscanf(star + 1, "%02X", &actual);
    TEST_ASSERT_EQUAL_HEX8(expected, (uint8_t)actual);
}

void test_telemetry_state_mapping(void) {
    flight_context_t ctx = {0};

    /* PAD_IDLE should output state=0 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "PYRO,0,0,") != NULL);

    /* ASCENT should output state=1 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, ASCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",1,") != NULL);
}

void test_telemetry_flags(void) {
    flight_context_t ctx = {0};
    ctx.pyro1_continuity_good = true;  /* bit 0 */
    ctx.pyro2_continuity_good = true;  /* bit 1 */
    ctx.pyros_armed = true;            /* bit 4 */
    /* flags = 0x13 */

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",13,") != NULL);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Helpers */
    RUN_TEST(test_pressure_to_altitude_cm);
    RUN_TEST(test_filter_pressure_init);
    RUN_TEST(test_filter_pressure_smoothing);
    RUN_TEST(test_buf_add);
    RUN_TEST(test_buf_add_wraps);

    /* Boot */
    RUN_TEST(test_boot_sequence_reaches_pad_idle);
    RUN_TEST(test_boot_calibrates_ground_pressure);
    RUN_TEST(test_boot_no_sensor);
    RUN_TEST(test_boot_i2c_settle_waits);

    /* PAD_IDLE */
    RUN_TEST(test_pad_idle_stays_on_ground);
    RUN_TEST(test_pad_idle_detects_ascent);
    RUN_TEST(test_pad_idle_continuity_check);

    /* ASCENT */
    RUN_TEST(test_ascent_tracks_max_altitude);
    RUN_TEST(test_ascent_arms_pyros_at_low_speed);
    RUN_TEST(test_ascent_detects_apogee);

    /* DESCENT */
    RUN_TEST(test_descent_detects_landing);

    /* LANDED */
    RUN_TEST(test_landed_stays_landed);

    /* Telemetry */
    RUN_TEST(test_telemetry_format);
    RUN_TEST(test_telemetry_seq_increments);
    RUN_TEST(test_telemetry_checksum);
    RUN_TEST(test_telemetry_state_mapping);
    RUN_TEST(test_telemetry_flags);

    return UNITY_END();
}
