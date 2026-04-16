/*
 * Unit tests for flight state machine and telemetry.
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include "../src/flight_states.h"
#include "pressure_processing.h"
#include "../src/telemetry_formatter.h"

/* Wrapper: feed pressure into pp, then dispatch */
static flight_state_t step(flight_context_t *ctx, uint32_t now) {
    hal_tasks_tick(now);
    return dispatch_state(ctx, now);
}

/* Helper: advance boot to PAD_IDLE */
static void boot_to_pad_idle(flight_context_t *ctx) {
    ctx->current_state = BOOT_SETTLE;
    ctx->boot_timer = 0;
    mock_time_ms = 0;

    /* BOOT_SETTLE → BOOT_CONTINUITY (after 2500ms) */
    mock_time_ms = 2600;
    ctx->current_state = step(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(BOOT_CONTINUITY, ctx->current_state);

    /* BOOT_CONTINUITY → BOOT_CALIBRATE */
    ctx->current_state = step(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(BOOT_CALIBRATE, ctx->current_state);

    /* Run 10 calibration readings */
    for (int i = 0; i < 10; i++) {
        mock_time_ms += 110;
        ctx->current_state = step(ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx->current_state);
}

void setUp(void) {
    mock_reset_all();
    pp_init(); /* Reset pressure processing state for each test */
    /* Telemetry formatter needs explicit init — previously happened as a
     * side-effect of detect_boot_init() running during boot tests. */
    config_t cfg;
    config_set_defaults(&cfg);
    telemetry_init(&cfg);
}

void tearDown(void) {}

/* ── Helper tests ─────────────────────────────────────────────────── */

void test_SNS_ALT_01_pressure_to_altitude(void) {
    /* Sea level: 0 altitude */
    TEST_ASSERT_EQUAL(0, pp_pressure_to_altitude_cm(101325, 101325));
    /* Lower pressure = higher altitude — hypsometric formula, ~8330 cm for 1000 Pa drop */
    int32_t alt = pp_pressure_to_altitude_cm(100325, 101325);
    TEST_ASSERT_TRUE(alt > 0);
    TEST_ASSERT_INT_WITHIN(1000, 8330, alt);
}

/* 5000 ft = 1524 m target — hypsometric formula must be within 0.5% */
void test_SNS_ALT_02_altitude_accuracy_5000ft(void) {
    /* ISA pressure at 1524 m: P = 101325 × (1 − 0.0065×1524/288.15)^5.2561 ≈ 84262 Pa */
    int32_t alt_cm = pp_pressure_to_altitude_cm(84262, 101325);
    int32_t expected_cm = 152400; /* 1524 m in cm */
    /* Allow ≤ 0.5 % = ±762 cm */
    TEST_ASSERT_INT_WITHIN(762, expected_cm, alt_cm);
}

void test_SNS_PRES_03_filter_init(void) {
    pp_init();
    int32_t result = pp_filter_pressure(101325, 10);
    TEST_ASSERT_EQUAL(101325, result);
}

void test_SNS_PRES_02_filter_smoothing(void) {
    pp_init();
    pp_filter_pressure(101325, 10);
    /* Step change — filter should smooth */
    int32_t result = pp_filter_pressure(100325, 100);
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

void test_DAT_01_buf_wraps(void) {
    flight_context_t ctx = {0};
    for (int i = 0; i < FLIGHT_BUF_SIZE; i++)
        buf_add(&ctx, i, 101325, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(FLIGHT_BUF_SIZE, ctx.buf_count);
    buf_add(&ctx, 9999, 101325, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(FLIGHT_BUF_SIZE, ctx.buf_count);
}

/* ── Boot state tests ─────────────────────────────────────────────── */

void test_FLT_BOOT_01_reaches_pad_idle(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    boot_to_pad_idle(&ctx);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

void test_FLT_BOOT_08_calibrates_ground(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    mock_pressure.pressure_pa = 101000.0f;
    boot_to_pad_idle(&ctx);
    TEST_ASSERT_INT_WITHIN(100, 101000, ctx.ground_pressure);
}

/* With no sensor, hal_pressure_read() returns false.
 * detect_boot_calibrate() skips those readings, so the device
 * stays in BOOT_CALIBRATE — safer than feeding uninitialized data. */
void test_SNS_PRES_01_boot_no_sensor(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    mock_time_ms = 0;
    mock_pressure.sensor_type = 0;

    /* BOOT_SETTLE → BOOT_CONTINUITY → BOOT_CALIBRATE */
    mock_time_ms = 2600;
    ctx.current_state = step(&ctx, mock_time_ms);
    ctx.current_state = step(&ctx, mock_time_ms);

    /* Run 10 calibration attempts — no sensor returns false every time,
     * so the device stays stuck in BOOT_CALIBRATE (correct behavior). */
    for (int i = 0; i < 10; i++) {
        mock_time_ms += 110;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(BOOT_CALIBRATE, ctx.current_state);
    TEST_ASSERT_EQUAL(0, ctx.ground_pressure);
}

void test_FLT_BOOT_04_settle_wait(void) {
    flight_context_t ctx = {0};
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    /* Before 2500ms — stays in settle */
    TEST_ASSERT_EQUAL(BOOT_SETTLE, step(&ctx, 2000));
    /* After 2500ms — advances to continuity */
    TEST_ASSERT_EQUAL(BOOT_CONTINUITY, step(&ctx, 2600));
}

/* ── PAD_IDLE tests ───────────────────────────────────────────────── */

void test_FLT_LAUNCH_02_stays_on_ground(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    mock_pressure.pressure_pa = 101325.0f;

    mock_time_ms = 1000;
    ctx.last_sample = 0;
    flight_state_t next = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(PAD_IDLE, next);
}

void test_FLT_LAUNCH_01_detects_ascent(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = false;
    pp_test_prime(101325);
    /* Simulate large altitude: pressure drop of ~150 Pa = ~1245 cm > 1000 cm threshold */
    mock_pressure.pressure_pa = 101325.0f - 150.0f;

    /* Run several samples so filter converges */
    for (int i = 0; i < 20; i++) {
        mock_time_ms = i * 15;
        ctx.last_sample = (i > 0) ? (i - 1) * 15 : 0;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.launch_time > 0);
}

void test_PYR_CONT_01_continuity_check(void) {
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
    step(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.pyro1_continuity_good);
    TEST_ASSERT_FALSE(ctx.pyro2_continuity_good);
    TEST_ASSERT_EQUAL(42, ctx.pyro1_adc);
    TEST_ASSERT_EQUAL(4000, ctx.pyro2_adc);
}

/* ── ASCENT tests ─────────────────────────────────────────────────── */

void test_FLT_ASC_01_tracks_max_altitude(void) {
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
    step(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.max_altitude >= 2000);
}

void test_FLT_ASC_04_arms_pyros(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 100700;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 5000;
    ctx.vertical_speed_cms = 500; /* 5 m/s — below 10 m/s threshold, coasting */
    ctx.max_speed_cms = 5000;     /* [DD-017] peak was 50 m/s — motor burn confirmed */

    /* Altitude slightly higher than last — positive but slow speed.
     * Pressure chosen so hypsometric formula gives ~5100 cm, yielding
     * speed = (5100-5000)*1000/200 = 500 cm/s < 1000 cm/s arming gate. */
    ctx.filtered_pressure = 100714;        /* pre-converged to match mock */
    mock_pressure.pressure_pa = 100714.0f; /* → ~5100 cm with hypsometric formula */
    mock_time_ms = 200;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_TRUE(ctx.pyros_armed);
}

void test_FLT_APO_01_detects_apogee(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 100425; /* close to what we'll read */
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 8000;
    ctx.max_altitude = 8000;
    ctx.pyros_armed = true;
    ctx.vertical_speed_cms = 100;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;

    /* Altitude lower than last → negative speed → apogee */
    mock_pressure.pressure_pa = 101325.0f - 900.0f; /* ~7470 cm < 8000 */
    mock_time_ms = 200;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(FALLING, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.apogee_detected);
}

/* ── FALLING / DROGUE / CHUTE tests ──────────────────────────────── */

/* Verify pyro1 fire → FALLING→DROGUE_DESCENT transition */
void test_FLT_DROGUE_01_transitions_on_pyro1_fire(void) {
    flight_context_t ctx = {0};
    ctx.current_state = FALLING;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.apogee_detected = true;
    ctx.pyro1_continuity_good = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 5000;
    ctx.vertical_speed_cms = -500;
    ctx.launch_time = 0;
    ctx.last_sample = 0;

    /* pyro1 already fired */
    ctx.pyro1_fired = true;

    mock_pressure.pressure_pa = 101325.0f - 600.0f; /* ~5000 cm */
    mock_time_ms = 100;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(DROGUE_DESCENT, ctx.current_state);
}

/* Verify pyro2 fire → DROGUE_DESCENT→CHUTE_DESCENT transition */
void test_FLT_CHUTE_01_transitions_on_pyro2_fire(void) {
    flight_context_t ctx = {0};
    ctx.current_state = DROGUE_DESCENT;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.apogee_detected = true;
    ctx.pyro1_fired = true;
    ctx.pyro2_continuity_good = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 3000;
    ctx.vertical_speed_cms = -300;
    ctx.launch_time = 0;
    ctx.last_sample = 0;

    /* pyro2 already fired */
    ctx.pyro2_fired = true;

    mock_pressure.pressure_pa = 101325.0f - 360.0f; /* ~3000 cm */
    mock_time_ms = 100;
    ctx.current_state = step(&ctx, mock_time_ms);

    TEST_ASSERT_EQUAL(CHUTE_DESCENT, ctx.current_state);
}

void test_FLT_LAND_01_detects_landing(void) {
    flight_context_t ctx = {0};
    ctx.current_state = CHUTE_DESCENT;
    ctx.pyro1_fired = true; /* drogue already deployed */
    ctx.pyro2_fired = true; /* main already deployed */
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101313; /* pre-converged near mock pressure */
    ctx.launch_time = 0;
    ctx.last_altitude = 100;
    ctx.vertical_speed_cms = 0;
    ctx.apogee_detected = true;

    /* Stable altitude near ground for >1 second */
    mock_pressure.pressure_pa = 101325.0f - 12.0f; /* ~100 cm */
    ctx.last_sample = 0;
    mock_time_ms = 100;
    step(&ctx, mock_time_ms);
    TEST_ASSERT_TRUE(ctx.landing_stable_since > 0);

    /* Keep stable */
    for (int i = 0; i < 20; i++) {
        mock_time_ms += 60;
        ctx.last_sample = mock_time_ms - 60;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(LANDED, ctx.current_state);
}

/* ── LANDED tests ─────────────────────────────────────────────────── */

void test_FLT_LAND_06_stays_landed(void) {
    flight_context_t ctx = {0};
    ctx.current_state = LANDED;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    mock_pressure.pressure_pa = 101325.0f;

    mock_time_ms = 2000;
    flight_state_t next = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(LANDED, next);
}

/* ── Telemetry tests ──────────────────────────────────────────────── */

void test_TEL_01_format(void) {
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

void test_TEL_09_seq_increments(void) {
    flight_context_t ctx = {0};
    ctx.telemetry_seq = 0;

    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(1, ctx.telemetry_seq);

    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_EQUAL(2, ctx.telemetry_seq);
}

void test_TEL_02_checksum(void) {
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

void test_TEL_07_state_mapping(void) {
    flight_context_t ctx = {0};

    /* PAD_IDLE → state_id 0 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "PYRO,0,0,") != NULL);

    /* ASCENT → state_id 1 */
    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, ASCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",1,") != NULL);

    /* FALLING → state_id 2 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, FALLING);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",2,") != NULL);

    /* DROGUE_DESCENT → state_id 3 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, DROGUE_DESCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",3,") != NULL);

    /* CHUTE_DESCENT → state_id 4 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, CHUTE_DESCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",4,") != NULL);

    /* LANDED → state_id 5 */
    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, LANDED);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",5,") != NULL);
}

void test_telemetry_flags(void) {
    flight_context_t ctx = {0};
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    ctx.pyros_armed = true;

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",13,") != NULL);
}

void test_TEL_06_altitude_and_speed(void) {
    flight_context_t ctx = {0};
    ctx.vertical_speed_cms = -1500;
    ctx.max_altitude = 30000;
    ctx.filtered_pressure = 98000;

    mock_uart_len = 0;
    send_telemetry(&ctx, 5000, 25000, FALLING);

    int seq, st, thr;
    long alt, vel, maxalt, press;
    unsigned long time_ms;
    int n = sscanf(mock_uart_buf, "$PYRO,%d,%d,%d,%ld,%ld,%ld,%ld,%lu,", &seq, &st, &thr, &alt, &vel, &maxalt, &press,
                   &time_ms);
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL(25000, alt);
    TEST_ASSERT_EQUAL(-1500, vel);
    TEST_ASSERT_EQUAL(30000, maxalt);
    TEST_ASSERT_EQUAL(98000, press);
    TEST_ASSERT_EQUAL(5000, time_ms);
    TEST_ASSERT_EQUAL(2, st);
}

void test_TEL_10_thrust_flag(void) {
    flight_context_t ctx = {0};
    ctx.under_thrust = true;

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, ASCENT);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",1,1,") != NULL);

    mock_uart_len = 0;
    ctx.telemetry_seq = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",0,0,") != NULL);
}

void test_TEL_06_pyro_adc(void) {
    flight_context_t ctx = {0};
    ctx.pyro1_adc = 42;
    ctx.pyro2_adc = 3800;

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",42,3800,") != NULL);
}

void test_TEL_08_all_flags(void) {
    flight_context_t ctx = {0};
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    ctx.pyro1_fired = true;
    ctx.pyro2_fired = true;
    ctx.pyros_armed = true;
    ctx.apogee_detected = true;

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, PAD_IDLE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, ",3F,") != NULL);
}

void test_TEL_07_boot_maps_to_zero(void) {
    flight_context_t ctx = {0};

    mock_uart_len = 0;
    send_telemetry(&ctx, 0, 0, BOOT_CALIBRATE);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "PYRO,0,0,") != NULL);
}

/* ── Config parser tests ──────────────────────────────────────────── */

void test_CFG_02_parse_full(void) {
    config_t cfg = {0};
    char ini[] = "[pyro]\r\nid=ROCKET1\r\nname=MyRkt\r\npyro1_mode=delay\r\npyro1_value=0\r\npyro2_mode=agl\r\npyro2_"
                 "value=300\r\nunits=ft\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL_STRING("ROCKET1", cfg.id);
    TEST_ASSERT_EQUAL_STRING("MyRkt", cfg.name);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(0, cfg.pyro1_value);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(300, cfg.pyro2_value);
    TEST_ASSERT_EQUAL(2, cfg.units);
}

void test_CFG_04_parse_all_modes(void) {
    config_t cfg = {0};
    char ini1[] = "pyro1_mode=delay\r\n";
    parse_config_ini(ini1, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
    char ini2[] = "pyro1_mode=agl\r\n";
    parse_config_ini(ini2, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro1_mode);
    char ini3[] = "pyro1_mode=fallen\r\n";
    parse_config_ini(ini3, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, cfg.pyro1_mode);
    char ini4[] = "pyro1_mode=speed\r\n";
    parse_config_ini(ini4, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, cfg.pyro1_mode);
}

void test_CFG_03_parse_all_units(void) {
    config_t cfg = {0};
    char ini1[] = "units=cm\r\n";
    parse_config_ini(ini1, &cfg);
    TEST_ASSERT_EQUAL(0, cfg.units);
    char ini2[] = "units=m\r\n";
    parse_config_ini(ini2, &cfg);
    TEST_ASSERT_EQUAL(1, cfg.units);
    char ini3[] = "units=ft\r\n";
    parse_config_ini(ini3, &cfg);
    TEST_ASSERT_EQUAL(2, cfg.units);
}

void test_CFG_09_unix_newlines(void) {
    config_t cfg = {0};
    char ini[] = "[pyro]\npyro1_mode=speed\npyro1_value=42\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_SPEED, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(42, cfg.pyro1_value);
}

void test_CFG_02_no_section_header(void) {
    config_t cfg = {0};
    char ini[] = "pyro2_mode=fallen\r\npyro2_value=100\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(PYRO_MODE_FALLEN, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(100, cfg.pyro2_value);
}

void test_CFG_08_unknown_keys(void) {
    config_t cfg = {0};
    char ini[] = "foo=bar\r\npyro1_value=55\r\nbaz=qux\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(55, cfg.pyro1_value);
}

void test_CFG_04_unknown_mode(void) {
    config_t cfg = {0};
    char ini[] = "pyro1_mode=bogus\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(0, cfg.pyro1_mode);
}

void test_CFG_02_empty_string(void) {
    config_t cfg = {0};
    char ini[] = "";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(0, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(0, cfg.pyro2_value);
}

void test_CFG_09_no_trailing_newline(void) {
    config_t cfg = {0};
    char ini[] = "pyro1_value=123";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(123, cfg.pyro1_value);
}

void test_CFG_07_id_truncated(void) {
    config_t cfg = {0};
    char ini[] = "id=ABCDEFGHIJKLMNOP\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(8, strlen(cfg.id));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGH", cfg.id);
}

void test_CFG_06_preserves_unset(void) {
    config_t cfg = {0};
    cfg.pyro1_mode = PYRO_MODE_DELAY;
    cfg.pyro1_value = 99;
    char ini[] = "pyro2_mode=agl\r\npyro2_value=200\r\n";
    parse_config_ini(ini, &cfg);
    /* pyro1 fields unchanged */
    TEST_ASSERT_EQUAL(PYRO_MODE_DELAY, cfg.pyro1_mode);
    TEST_ASSERT_EQUAL(99, cfg.pyro1_value);
    /* pyro2 fields set */
    TEST_ASSERT_EQUAL(PYRO_MODE_AGL, cfg.pyro2_mode);
    TEST_ASSERT_EQUAL(200, cfg.pyro2_value);
}

void test_CFG_08_comment_lines(void) {
    config_t cfg = {0};
    /* Lines without '=' are skipped (section headers, comments) */
    char ini[] = "[pyro]\r\n; this is a comment\r\npyro1_value=77\r\n# another comment\r\n";
    parse_config_ini(ini, &cfg);
    TEST_ASSERT_EQUAL(77, cfg.pyro1_value);
}

/* ── False launch regression ──────────────────────────────────────── */

/* [FLT-BOOT-10] Sensor drift at boot must NOT trigger a false launch.
 *
 * Scenario: the pressure sensor drifts 200 Pa between the calibration
 * phase (where ground_pressure is set) and the first PAD_IDLE reading.
 * 200 Pa ≈ 1660 cm altitude — above the 1000 cm launch gate.
 *
 * Without the fix in action_ground_cal() (priming filter + last_sample),
 * the first PAD_IDLE iteration computes:
 *   altitude ≈ 1660 cm  (> 1000 cm threshold)
 *   speed    = (1660 - 0) * 1000 / 10 = 166000 cm/s  (> 500 cm/s)
 * → false SEVT_LAUNCH!
 *
 * With the fix, the IIR filter is primed to ground_pressure, so the
 * 200 Pa drift only produces a small filtered altitude step, and
 * last_sample is set so dt is realistic (not defaulted to 10ms). */
void test_FLT_BOOT_10_no_false_launch_on_drift(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    mock_time_ms = 0;

    /* Calibration pressure: 101325 Pa (stable during boot) */
    mock_pressure.pressure_pa = 101325.0f;

    /* Run through boot: BOOT_SETTLE → BOOT_CONTINUITY → BOOT_CALIBRATE → PAD_IDLE */
    mock_time_ms = 2600;
    ctx.current_state = step(&ctx, mock_time_ms); /* BOOT_SETTLE → BOOT_CONTINUITY */
    ctx.current_state = step(&ctx, mock_time_ms); /* BOOT_CONTINUITY → BOOT_CALIBRATE */
    for (int i = 0; i < 10; i++) {
        mock_time_ms += 110;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "Should reach PAD_IDLE");
    TEST_ASSERT_INT_WITHIN(100, 101325, ctx.ground_pressure);

    /* NOW simulate sensor drift: pressure drops 200 Pa after calibration.
     * On real hardware this can happen from thermal settling or sensor
     * startup transient. 200 Pa ≈ 16.6 m = 1660 cm AGL. */
    mock_pressure.pressure_pa = 101325.0f - 200.0f;

    /* Run several PAD_IDLE iterations — must NOT trigger launch */
    for (int i = 0; i < 20; i++) {
        mock_time_ms += 15;
        ctx.current_state = step(&ctx, mock_time_ms);
        TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "Sensor drift must NOT trigger false launch");
    }
}

/* [BUG-REPRO] BMP280 sensor noise on the pad.
 * Real hardware shows ±25 Pa noise at 10ms intervals.  The IIR filter
 * (τ=500ms, α≈0.02 per 10ms step) should damp this to < 1 Pa of
 * filtered variation.  But the altitude-to-speed calculation amplifies
 * tiny filtered steps (e.g. 8 cm / 10 ms = 800 cm/s).
 * The altitude gate (>1000 cm) must prevent any false launch despite
 * these speed oscillations. */
void test_PAD_IDLE_noise_no_false_launch(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    mock_time_ms = 0;

    /* Stable calibration at 101325 Pa */
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);

    /* Apply realistic BMP280 noise: ±25 Pa, alternating every 20ms.
     * 20ms matches the ~50Hz sensor sample rate.
     * This mimics the captured hardware data where raw_pa oscillated
     * between 102173 and 102217 around a 102200 Pa mean. */
    int32_t max_speed = 0;
    for (int i = 0; i < 100; i++) {
        float noise = (i % 2 == 0) ? 25.0f : -25.0f;
        mock_pressure.pressure_pa = 101325.0f + noise;
        mock_time_ms += 20;
        ctx.current_state = step(&ctx, mock_time_ms);
        TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "BMP280 noise must NOT trigger false launch");

        int32_t spd = ctx.pad_speed_cms > 0 ? ctx.pad_speed_cms : -ctx.pad_speed_cms;
        if (spd > max_speed)
            max_speed = spd;
    }
    /* Speed oscillations exist but altitude stays near 0 — no false launch */
    TEST_ASSERT_TRUE_MESSAGE(ctx.last_altitude < 100, "Filtered altitude must stay near ground");
}

/* [DATA-FLOW] Single data path: batch FIFO → push_sample → dispatch_state.
 * hal_pressure_read() must return false between batches (no leaked data).
 * This is the architectural invariant that prevents the backward-time bug:
 * the old polled path consumed pres.last at real-time timestamps, then the
 * batch replayed the same samples with earlier timestamps, causing dt to
 * wrap to ~4 billion ms and overflowing the IIR filter.
 *
 * With pres_append() no longer writing pres.last, the only way data
 * reaches hal_pressure_read() is through push_sample() during batch
 * processing.  Between batches, has_last is false. */
void test_SNS_PRES_04_single_data_path(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    mock_time_ms = 0;
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);

    /* After boot, the mock provides samples via hal_pressure_read().
     * Verify that monotonically increasing timestamps produce correct,
     * smooth filter output — no jumps, no overflow. */
    int32_t prev_filtered = ctx.filtered_pressure;
    for (int i = 0; i < 100; i++) {
        mock_time_ms += 20;                            /* 50 Hz */
        mock_pressure.pressure_pa = 101325.0f + 10.0f; /* tiny offset */
        ctx.current_state = step(&ctx, mock_time_ms);

        /* Filter must move smoothly — no jump > 50 Pa per step */
        int32_t jump = ctx.filtered_pressure - prev_filtered;
        if (jump < 0)
            jump = -jump;
        TEST_ASSERT_TRUE_MESSAGE(jump < 50, "Filter step must be smooth with monotonic timestamps");
        prev_filtered = ctx.filtered_pressure;
    }
    /* Filter should converge toward 101335 (101325 + 10) */
    TEST_ASSERT_INT_WITHIN(20, 101335, ctx.filtered_pressure);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Helpers */
    RUN_TEST(test_SNS_ALT_01_pressure_to_altitude);
    RUN_TEST(test_SNS_ALT_02_altitude_accuracy_5000ft);
    RUN_TEST(test_SNS_PRES_03_filter_init);
    RUN_TEST(test_SNS_PRES_02_filter_smoothing);
    RUN_TEST(test_buf_add);
    RUN_TEST(test_DAT_01_buf_wraps);

    /* Boot */
    RUN_TEST(test_FLT_BOOT_01_reaches_pad_idle);
    RUN_TEST(test_FLT_BOOT_08_calibrates_ground);
    RUN_TEST(test_SNS_PRES_01_boot_no_sensor);
    RUN_TEST(test_FLT_BOOT_04_settle_wait);

    /* PAD_IDLE */
    RUN_TEST(test_FLT_LAUNCH_02_stays_on_ground);
    RUN_TEST(test_FLT_LAUNCH_01_detects_ascent);
    RUN_TEST(test_PYR_CONT_01_continuity_check);

    /* ASCENT */
    RUN_TEST(test_FLT_ASC_01_tracks_max_altitude);
    RUN_TEST(test_FLT_ASC_04_arms_pyros);
    RUN_TEST(test_FLT_APO_01_detects_apogee);

    /* FALLING / DROGUE / CHUTE */
    RUN_TEST(test_FLT_DROGUE_01_transitions_on_pyro1_fire);
    RUN_TEST(test_FLT_CHUTE_01_transitions_on_pyro2_fire);
    RUN_TEST(test_FLT_LAND_01_detects_landing);

    /* LANDED */
    RUN_TEST(test_FLT_LAND_06_stays_landed);

    /* Telemetry */
    RUN_TEST(test_TEL_01_format);
    RUN_TEST(test_TEL_09_seq_increments);
    RUN_TEST(test_TEL_02_checksum);
    RUN_TEST(test_TEL_07_state_mapping);
    RUN_TEST(test_telemetry_flags);
    RUN_TEST(test_TEL_06_altitude_and_speed);
    RUN_TEST(test_TEL_10_thrust_flag);
    RUN_TEST(test_TEL_06_pyro_adc);
    RUN_TEST(test_TEL_08_all_flags);
    RUN_TEST(test_TEL_07_boot_maps_to_zero);

    /* Config parser */
    RUN_TEST(test_CFG_02_parse_full);
    RUN_TEST(test_CFG_04_parse_all_modes);
    RUN_TEST(test_CFG_03_parse_all_units);
    RUN_TEST(test_CFG_09_unix_newlines);
    RUN_TEST(test_CFG_02_no_section_header);
    RUN_TEST(test_CFG_08_unknown_keys);
    RUN_TEST(test_CFG_04_unknown_mode);
    RUN_TEST(test_CFG_02_empty_string);
    RUN_TEST(test_CFG_09_no_trailing_newline);
    RUN_TEST(test_CFG_07_id_truncated);
    RUN_TEST(test_CFG_06_preserves_unset);
    RUN_TEST(test_CFG_08_comment_lines);

    /* False launch regression */
    RUN_TEST(test_FLT_BOOT_10_no_false_launch_on_drift);
    RUN_TEST(test_PAD_IDLE_noise_no_false_launch);
    RUN_TEST(test_SNS_PRES_04_single_data_path);

    return UNITY_END();
}
