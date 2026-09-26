/*
 * Unit tests for flight state machine and telemetry.
 */
#include "unity.h"
#include "../src/buzzer.h"
#include "../src/beep_store.h"

/* The launch trigger, for the assertion below. Mirrors flight_states.c. */
#define LAUNCH_ALT_CM_FOR_TEST 3048

beep_reason_t beep_reason_for_diag(uint16_t diag);
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
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

    /* BOOT_SETTLE → BOOT_SENSOR (after 2500ms) */
    mock_time_ms = 2600;
    ctx->current_state = step(ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(BOOT_SENSOR, ctx->current_state);

    /* BOOT_SENSOR → BOOT_CONTINUITY. The sensor is tested before the pyros,
       so a board with no sensor never reaches the continuity beep. */
    ctx->sensor_type = 2;
    ctx->fs_ok = true;
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
    /* The telemetry formatter needs an explicit init here, rather than as a
     * side-effect of detect_boot_init() running during boot tests.
     *
     * MUST be static: telemetry_init() borrows the pointer rather than
     * copying, so a stack local here leaves s_cfg dangling the moment
     * setUp() returns. telemetry_state() then early-returns on a garbage
     * telem_format and every $PYRO test fails -- but only when the dead
     * stack happens to read wrong, so it alternated between passing and
     * failing on identical code and flaked CI at random. */
    static config_t cfg;
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
    TEST_ASSERT_EQUAL(BOOT_SENSOR, ctx.current_state);

    /* No sensor answered at init. The board must NOT proceed to the pyro
       test and beep "all good" -- it cannot measure altitude, so it can
       never detect a launch. It goes to FAULT and says so. */
    ctx.sensor_type = 0;
    ctx.fs_ok = true;
    ctx.current_state = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL_MESSAGE(FAULT, ctx.current_state, "a board with no sensor must fault, not idle");
    TEST_ASSERT_TRUE_MESSAGE(ctx.diag & DIAG_SENSOR_FAIL, "the diagnosis names the sensor");
    TEST_ASSERT_EQUAL_MESSAGE(BR_SYSTEM_FAILURE, beep_reason_for_diag(ctx.diag),
                              "and a dead sensor means safe it and leave the pad");

    /* Terminal: nothing recovers from it. */
    for (int i = 0; i < 10; i++) {
        mock_time_ms += 110;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(FAULT, ctx.current_state);
    TEST_ASSERT_EQUAL(0, ctx.ground_pressure);
}

void test_FLT_BOOT_04_settle_wait(void) {
    flight_context_t ctx = {0};
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    /* Before 2500ms — stays in settle */
    TEST_ASSERT_EQUAL(BOOT_SETTLE, step(&ctx, 2000));
    /* After 2500ms — advances to the sensor test */
    TEST_ASSERT_EQUAL(BOOT_SENSOR, step(&ctx, 2600));
}

/* [FLT-BOOT-13] A sensor that answered at power-up but gives calibration no
 * samples: FAULT after 10 s, not a board that beeps "all good" and can never
 * see a launch. */
void test_FLT_BOOT_13_no_calibration_samples_is_fault(void) {
    flight_context_t ctx = {0};
    ctx.current_state = BOOT_CALIBRATE;
    ctx.boot_timer = 0;
    pp_start_cal();
    mock_pressure.sensor_type = 0; /* nothing is fed */
    TEST_ASSERT_EQUAL(BOOT_CALIBRATE, step(&ctx, 9900));
    TEST_ASSERT_EQUAL(FAULT, step(&ctx, 10000));
    TEST_ASSERT_TRUE(ctx.diag & DIAG_SENSOR_FAIL);
}

/* [FLT-BOOT-14] No filesystem: no log, no config, no marker. FAULT. */
void test_FLT_BOOT_14_no_filesystem_is_fault(void) {
    flight_context_t ctx = {0};
    ctx.current_state = BOOT_SENSOR;
    ctx.sensor_type = 2;
    ctx.fs_ok = false;
    TEST_ASSERT_EQUAL(FAULT, step(&ctx, 100));
    TEST_ASSERT_TRUE(ctx.diag & DIAG_FS_FAIL);
}

/* [FLT-BOOT-02, FLT-BOOT-03] The configuration is read at power-up, and a
 * board with none gets the defaults written out. The RP2040's
 * hal_config_load() is the same code as the host's. */
void test_FLT_BOOT_02_reads_config_at_boot(void) {
    const char *ini = "[pyro]\r\npyro2_mode=agl\r\npyro2_value=123\r\n";
    TEST_ASSERT_EQUAL(0, hal_fs_write_file("config.ini", ini, (int)strlen(ini)));
    static flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_EQUAL_UINT16(123, ctx.config.pyro2_value);
}

void test_FLT_BOOT_03_writes_default_config(void) {
    char buf[64];
    TEST_ASSERT_TRUE(hal_fs_read_file("config.ini", buf, (int)sizeof(buf)) <= 0);
    static flight_context_t ctx;
    flight_init(&ctx);
    TEST_ASSERT_TRUE_MESSAGE(hal_fs_read_file("config.ini", buf, (int)sizeof(buf)) > 0,
                             "a board with no config.ini gets one");
}

/* ── PAD_IDLE tests ───────────────────────────────────────────────── */

void test_FLT_LAUNCH_02_stays_on_ground(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
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
    pp_test_prime(101325);
    /* Launch is now 100 ft (3048 cm) above the ground reference, not 10 m.
       ~12 Pa per metre near sea level, so 30.5 m needs about 370 Pa; 600 Pa
       (~5000 cm) clears it without depending on the exact lapse rate. The
       climb continues at about 33 m/s: the trigger must hold for
       LAUNCH_HOLD_MS, and only a rocket still climbing holds it. */
    for (int i = 0; i < 40; i++) {
        mock_time_ms = i * 15;
        mock_pressure.pressure_pa = 101325.0f - 600.0f - 0.4f * (float)mock_time_ms;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state);
    /* The very first sample was already above 50 cm, so it is T+0. */
    TEST_ASSERT_EQUAL_UINT32(0, ctx.launch_time);
}

/* ── The ground reference ─────────────────────────────────────────
 *
 * A 5-second rolling mean of the filtered PRESSURE, frozen at launch. MK1C
 * false-launched on a bench from weather drift under the old 10 m trigger, so
 * these hold the two properties that prevent it: the reference follows slow
 * drift, and the trigger is 100 ft above wherever the reference has got to. */

static void feed_pad(flight_context_t *ctx, float pa, uint32_t from_ms, uint32_t to_ms, uint32_t step_ms) {
    for (uint32_t t = from_ms; t <= to_ms; t += step_ms) {
        mock_time_ms = t;
        mock_pressure.pressure_pa = pa;
        ctx->current_state = step(ctx, t);
    }
}

void test_GND_CAL_01_reference_follows_slow_drift(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;

    /* A weather front: 30 Pa over ten seconds, well inside the band a real
       launch leaves immediately. The reference should have followed it. */
    for (int i = 0; i <= 500; i++) {
        feed_pad(&ctx, 101325.0f - (float)i * 0.06f, (uint32_t)i * 20, (uint32_t)i * 20, 20);
    }
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "slow drift must not read as a launch");
    TEST_ASSERT_INT_WITHIN_MESSAGE(15, 101295, pp_ground_pressure(), "the reference should have tracked the drift");
}

void test_GND_CAL_02_reference_stops_tracking_when_the_rocket_moves(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;
    feed_pad(&ctx, 101325.0f, 0, 400, 20);
    int32_t before = pp_ground_pressure();

    /* A launch: far outside the band, immediately. The reference must hold
       still rather than climb with the rocket. */
    feed_pad(&ctx, 101325.0f - 600.0f, 420, 800, 20);
    TEST_ASSERT_INT_WITHIN_MESSAGE(3, before, pp_ground_pressure(), "the reference must not follow a climbing rocket");
}

void test_FLT_LAUNCH_08_ten_metres_is_no_longer_enough(void) {
    /* The old trigger. MK1C reached it on a bench from pressure drift. */
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;

    /* ~250 Pa is about 21 m (69 ft): comfortably past the old 10 m trigger
       and comfortably short of 100 ft, so this discriminates between them
       rather than sitting on either boundary. Run long enough for the IIR to
       settle, and for the speed condition to have been satisfied on the way
       -- otherwise this would pass merely because nothing moved. */
    feed_pad(&ctx, 101325.0f - 250.0f, 0, 2000, 20);
    TEST_ASSERT_GREATER_THAN_MESSAGE(1000, ctx.last_altitude, "the climb must clear the OLD 10 m threshold");
    TEST_ASSERT_LESS_THAN_MESSAGE(LAUNCH_ALT_CM_FOR_TEST, ctx.last_altitude, "and stay under the new one");
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "10 m must no longer trip the launch detector");
}

void test_FLT_LAUNCH_09_freezing_keeps_the_hundred_feet(void) {
    flight_context_t ctx = {0};
    ctx.config = (config_t){"TEST", "TEST", 1, 300, 1, 150};
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;
    feed_pad(&ctx, 101325.0f, 0, 400, 20);
    int32_t pad_ref = pp_ground_pressure();

    /* The 500 ms IIR reaches only ~63% of a step in its first time constant,
       so give it long enough to clear 100 ft rather than graze it. */
    feed_pad(&ctx, 101325.0f - 900.0f, 420, 1600, 20);
    TEST_ASSERT_EQUAL_MESSAGE(ASCENT, ctx.current_state, "900 Pa is well past 100 ft");

    /* Frozen, not snapped: the reference is still the pad's, so the altitude
       the rocket had already gained is not thrown away. */
    TEST_ASSERT_FALSE_MESSAGE(pp_ground_tracking(), "launch must freeze the reference");
    TEST_ASSERT_INT_WITHIN_MESSAGE(3, pad_ref, pp_ground_pressure(), "and freeze it at the PAD value");
    TEST_ASSERT_GREATER_THAN_MESSAGE(LAUNCH_ALT_CM_FOR_TEST, ctx.last_altitude,
                                     "altitude at launch should read the height actually reached, not zero");
}

void test_PYR_CONT_01_continuity_check(void) {
    flight_context_t ctx = {0};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
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
    pp_test_prime(101325);
    ctx.filtered_pressure = 100700;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 5000;
    ctx.last_height = 5000;
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

/* [FLT-ASC-07, DD-017] The burn is confirmed by a peak over 10 m/s of true
 * speed: the fit's speed is not the filtered one DD-017 once halved. */
static bool arms_after_peak(int32_t peak_cms) {
    setUp();
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    pp_test_prime(101325);
    ctx.filtered_pressure = 100714;
    ctx.last_altitude = 5000;
    ctx.last_height = 5000;
    ctx.vertical_speed_cms = 500;
    ctx.max_speed_cms = peak_cms;
    mock_pressure.pressure_pa = 100714.0f; /* 5 m/s up, as in test_FLT_ASC_04_arms_pyros */
    mock_time_ms = 200;
    ctx.current_state = step(&ctx, mock_time_ms);
    return ctx.pyros_armed;
}

void test_FLT_ASC_07_arms_after_ten_metres_a_second(void) {
    TEST_ASSERT_FALSE_MESSAGE(arms_after_peak(900), "a 9 m/s peak is no burn");
    TEST_ASSERT_TRUE_MESSAGE(arms_after_peak(1100), "an 11 m/s peak is");
}

/* Over the top in free fall, about 75 m up: apogee once clean fits show the
 * pressure rising and it has risen 1.0001 above its lowest, 10 Pa -- 0.41 s
 * after the peak, 0.84 m below it [FLT-APO-01, DD-048]. */
void test_FLT_APO_01_detects_apogee(void) {
    flight_context_t ctx = {0};
    ctx.current_state = ASCENT;
    ctx.ground_pressure = 101325;
    pp_test_prime(101325);
    ctx.filtered_pressure = 100425; /* close to what we'll read */
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    ctx.last_altitude = 7500;
    ctx.last_height = 7500;
    ctx.max_altitude = 7500;
    ctx.pyros_armed = true;
    ctx.vertical_speed_cms = 100;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;

    const uint32_t peak = 1200;
    uint32_t apogee = 0;
    for (uint32_t t = 200; t <= 2400 && ctx.current_state == ASCENT; t += 20) {
        float s = ((float)t - (float)peak) / 1000.0f;
        mock_pressure.pressure_pa = 100425.0f + 0.5f * 117.7f * s * s; /* rho g g at 75 m */
        mock_time_ms = t;
        ctx.current_state = step(&ctx, mock_time_ms);
        if (ctx.apogee_detected && apogee == 0)
            apogee = ctx.last_sample;
    }

    TEST_ASSERT_EQUAL(FALLING, ctx.current_state);
    TEST_ASSERT_TRUE(ctx.apogee_detected);
    char msg[64];
    snprintf(msg, sizeof(msg), "apogee on the sample of %u ms", (unsigned)apogee);
    TEST_ASSERT_TRUE_MESSAGE(apogee >= peak + 350u && apogee <= peak + 480u, msg);
}

/* ── FALLING / DROGUE / CHUTE tests ──────────────────────────────── */

/* [FLT-DESC-01] The descent phase comes from the rate, so these two can no
 * longer be single-step tests: a phase needs its dwell. Feeding a steady rate
 * is the point -- under the old contract merely setting pyro1_fired advanced
 * the machine, which is what let a commanded-but-dead canopy look deployed. */
#define PA_PER_CM 0.12f

static void descend_steady(flight_context_t *ctx, int32_t start_cm, int32_t rate_cms, uint32_t ms) {
    for (uint32_t t = 0; t <= ms; t += 20) {
        int32_t alt_cm = start_cm - (int32_t)((int64_t)rate_cms * (int64_t)t / 1000);
        mock_pressure.pressure_pa = 101325.0f - (float)alt_cm * PA_PER_CM;
        mock_time_ms += 20;
        ctx->current_state = step(ctx, mock_time_ms);
    }
}

/* A steady 20 m/s is a drogue, and says so without any pyro having fired. */
void test_FLT_DESC_03_drogue_phase_from_rate(void) {
    flight_context_t ctx = {0};
    ctx.current_state = FALLING;
    ctx.ground_pressure = 101325;
    ctx.apogee_detected = true;
    ctx.config.landing_timeout = 0; /* under test here: phase, not landing */
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 50000;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    mock_time_ms = 0;

    descend_steady(&ctx, 50000, 2000, 4000);

    TEST_ASSERT_EQUAL_MESSAGE(DROGUE_DESCENT, ctx.current_state,
                              "a steady 20 m/s descent is a drogue, whatever the firing log says");
    TEST_ASSERT_FALSE_MESSAGE(ctx.pyro1_fired, "the phase must not depend on a pyro having fired");
}

/* And a steady 5 m/s is the main. */
void test_FLT_DESC_04_chute_phase_from_rate(void) {
    flight_context_t ctx = {0};
    ctx.current_state = DROGUE_DESCENT;
    ctx.ground_pressure = 101325;
    ctx.apogee_detected = true;
    ctx.config.landing_timeout = 0;
    pp_test_prime(101325);
    ctx.filtered_pressure = 101325;
    ctx.last_altitude = 30000;
    ctx.launch_time = 0;
    ctx.last_sample = 0;
    mock_time_ms = 0;

    descend_steady(&ctx, 30000, 500, 4000);

    TEST_ASSERT_EQUAL_MESSAGE(CHUTE_DESCENT, ctx.current_state,
                              "a steady 5 m/s descent is the main, whatever the firing log says");
    TEST_ASSERT_FALSE_MESSAGE(ctx.pyro2_fired, "the phase must not depend on a pyro having fired");
}

void test_FLT_LAND_01_detects_landing(void) {
    flight_context_t ctx = {0};
    ctx.current_state = CHUTE_DESCENT;
    ctx.pyro1_fired = true; /* drogue already deployed */
    ctx.pyro2_fired = true; /* main already deployed */
    ctx.ground_pressure = 101325;
    pp_test_prime(101325);
    /* This board is descending, so its ground reference froze at launch.
       Leaving it tracking would have the reference chase the rocket down. */
    pp_ground_track(false);
    ctx.filtered_pressure = 101313; /* pre-converged near mock pressure */
    ctx.launch_time = 0;
    ctx.last_altitude = 100;
    ctx.last_height = 100;
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

/* [FLT-LAUNCH-02] Sensor drift at boot must NOT trigger a false launch.
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

    /* Boot: SETTLE → SENSOR → CONTINUITY → CALIBRATE → PAD_IDLE */
    ctx.sensor_type = 2;
    ctx.fs_ok = true;
    mock_time_ms = 2600;
    ctx.current_state = step(&ctx, mock_time_ms); /* SETTLE → SENSOR     */
    ctx.current_state = step(&ctx, mock_time_ms); /* SENSOR → CONTINUITY */
    ctx.current_state = step(&ctx, mock_time_ms); /* CONTINUITY → CALIBRATE */
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
 * This invariant is what keeps timestamps moving forward. Two paths
 * consuming pres.last would let the batch replay samples the polled path had
 * already taken at real-time timestamps, so dt would wrap to about 4 billion
 * ms and overflow the IIR filter.
 *
 * pres_append() must not write pres.last. Data then reaches
 * hal_pressure_read() only through push_sample() during batch processing,
 * and has_last is false between batches. */
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

/* ── Code review 2026-09-24 ───────────────────────────────────────── */

/* Sit on the pad at ground pressure, sampling at the sensor's 50 Hz. */
static void pad_run(flight_context_t *ctx, uint32_t ms) {
    uint32_t end = mock_time_ms + ms;
    while (mock_time_ms < end) {
        mock_time_ms += 20;
        mock_pressure.pressure_pa = 101325.0f;
        ctx->current_state = step(ctx, mock_time_ms);
    }
}

/* The time field of the last $PYRO sentence sent, or -1 when there is none. */
static long last_pyro_time_ms(void) {
    const char *last = NULL;
    for (const char *p = mock_uart_buf; (p = strstr(p, "$PYRO,")) != NULL; p++) {
        last = p;
    }
    if (!last) {
        return -1;
    }
    int seq, st, thr;
    long alt, vel, maxalt, press;
    unsigned long ms;
    if (sscanf(last, "$PYRO,%d,%d,%d,%ld,%ld,%ld,%ld,%lu,", &seq, &st, &thr, &alt, &vel, &maxalt, &press, &ms) != 8) {
        return -1;
    }
    return (long)ms;
}

static int count_pyro_sentences(void) {
    int n = 0;
    for (const char *p = mock_uart_buf; (p = strstr(p, "$PYRO,")) != NULL; p++) {
        n++;
    }
    return n;
}

/* [PYR-CONT-01, FLT-BOOT-15, REV-04] A lead that lets go while the rocket
 * waits on the pad must change what the buzzer says, and a fault fixed
 * without a power cycle must stop being reported. */
void test_REV04_pad_fault_after_boot_is_announced(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);
    pad_run(&ctx, 1500);
    TEST_ASSERT_EQUAL_MESSAGE(BR_OK_TO_FLY, ctx.last_reason, "a clean board says OK to fly");

    mock_pyro.p1_good = false;
    mock_pyro.p1_open = true;
    pad_run(&ctx, 2500);
    TEST_ASSERT_FALSE(ctx.pyro1_continuity_good);
    TEST_ASSERT_TRUE_MESSAGE(ctx.diag & DIAG_P1_OPEN, "the open lead must reach the diagnosis");
    TEST_ASSERT_EQUAL_MESSAGE(BR_CHECK_PYRO_1, ctx.last_reason, "and the buzzer must stop saying OK to fly");

    mock_pyro.p1_good = true;
    mock_pyro.p1_open = false;
    pad_run(&ctx, 2500);
    TEST_ASSERT_EQUAL_MESSAGE(0, ctx.diag & DIAG_PYRO_ANY, "a fixed lead is no longer a fault");
    TEST_ASSERT_EQUAL_MESSAGE(BR_OK_TO_FLY, ctx.last_reason, "and the buzzer says so without a power cycle");
}

/* A channel the operator disabled has nothing connected by design. Calling
 * that an open igniter sends them to the rocket for nothing, every time. */
void test_REV_NEW_disabled_channel_is_not_a_fault(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_NONE;
    mock_pyro.p1_good = false;
    mock_pyro.p1_open = true;
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);
    pad_run(&ctx, 1500);
    TEST_ASSERT_EQUAL_MESSAGE(0, ctx.diag & (DIAG_P1_OPEN | DIAG_P1_SHORT), "a disabled channel reported a fault");
    TEST_ASSERT_EQUAL(BR_OK_TO_FLY, ctx.last_reason);
}

/* [FLT-LAUNCH-03, REV-07] T+0 is the first sample above 50 cm, not the
 * moment the detector tripped a hundred feet later. */
void test_REV07_launch_backdates_to_first_rise(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;
    pad_run(&ctx, 2000);

    /* T+0 is the first reading above 50 cm [FLT-LAUNCH-03]: the truth passes
     * it here, and readings come every 20 ms. */
    uint32_t first_rise_ms = 0, detected_ms = 0;
    for (uint32_t t = 1; t <= 6000 && ctx.current_state == PAD_IDLE; t++) {
        float s = (float)t / 1000.0f;
        float alt_m = 25.0f * s * s; /* 50 m/s^2 off the rail */
        mock_time_ms += 1;
        mock_pressure.pressure_pa = 101325.0f * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
        ctx.current_state = step(&ctx, mock_time_ms);
        if (first_rise_ms == 0 && alt_m > 0.5f)
            first_rise_ms = mock_time_ms;
        if (ctx.current_state == ASCENT) {
            detected_ms = mock_time_ms;
        }
    }
    TEST_ASSERT_EQUAL_MESSAGE(ASCENT, ctx.current_state, "the climb must be detected");
    TEST_ASSERT_NOT_EQUAL(0, first_rise_ms);
    char m[128];
    snprintf(m, sizeof(m), "first rise %u, detected %u, launch_time %u", first_rise_ms, detected_ms, ctx.launch_time);
    TEST_ASSERT_GREATER_THAN_MESSAGE(300, detected_ms - first_rise_ms, m);
    TEST_ASSERT_TRUE_MESSAGE(ctx.launch_time >= first_rise_ms && ctx.launch_time <= first_rise_ms + 20u, m);
}

/* [TEL-05, REV-08] A board that failed its power-up test is not on the pad
 * waiting to fly. The ground-station contract has no state for it, so it
 * sends no $PYRO sentence at all -- state 0 would read as "ready". */
void test_REV08_fault_sends_no_state_sentence(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = FAULT;
    ctx.diag = DIAG_SENSOR_FAIL;
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    for (uint32_t t = 0; t < 6000; t += 10) {
        flight_update_outputs(&ctx, 100000 + t);
    }
    TEST_ASSERT_NULL_MESSAGE(strstr(mock_uart_buf, "$PYRO,"), "a faulted board must not report PAD_IDLE");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, "!FAULT sensor_fail"), "but a console is told why");

    ctx.current_state = BOOT_SENSOR;
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    for (uint32_t t = 0; t < 3000; t += 10) {
        flight_update_outputs(&ctx, 200000 + t);
    }
    TEST_ASSERT_NULL_MESSAGE(strstr(mock_uart_buf, "$PYRO,"), "nor may a board still booting");
}

/* [WEB-UI-04, REV-09] Flight time stops at the landing. Read ten minutes
 * later it is still the flight, not the time since launch. */
void test_REV09_flight_time_freezes_at_landing(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = CHUTE_DESCENT;
    ctx.pyro1_fired = true;
    ctx.pyro2_fired = true;
    ctx.apogee_detected = true;
    pp_test_prime(101325);
    pp_ground_track(false);
    ctx.ground_pressure = 101325;
    ctx.launch_time = 1000;
    ctx.last_altitude = 100;
    mock_pressure.pressure_pa = 101325.0f - 12.0f;
    mock_time_ms = 30000;
    ctx.last_sample = mock_time_ms;
    while (ctx.current_state != LANDED && mock_time_ms < 40000) {
        mock_time_ms += 20;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL(LANDED, ctx.current_state);
    uint32_t landed_at = mock_time_ms;

    mock_uart_len = 0;
    flight_update_outputs(&ctx, landed_at + 1000);
    long at_landing = last_pyro_time_ms();
    mock_uart_len = 0;
    flight_update_outputs(&ctx, landed_at + 600000);
    long ten_minutes_on = last_pyro_time_ms();
    TEST_ASSERT_GREATER_THAN(0, at_landing);
    TEST_ASSERT_EQUAL_MESSAGE(at_landing, ten_minutes_on, "flight time kept counting after the landing");
    TEST_ASSERT_INT_WITHIN(100, (long)(landed_at - ctx.launch_time), at_landing);
}

/* [SYS-DEPLOY-01, DAT-04, REV-03] A board that takes the fire call and
 * energises nothing has not deployed anything, and must not say it has. */
void test_REV03_refused_fire_is_not_recorded_as_fired(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_DELAY;
    ctx.config.pyro1_value = 0;
    ctx.config.pyro2_mode = PYRO_MODE_NONE;
    ctx.config.landing_timeout = 0;
    ctx.current_state = FALLING;
    ctx.apogee_detected = true;
    ctx.pyros_armed = true;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    pp_test_prime(101325);
    pp_ground_track(false);
    ctx.last_altitude = 50000;
    mock_time_ms = 0;
    ctx.apogee_time = 0;
    mock_pyro.refuse_fire = true;
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';

    descend_steady(&ctx, 50000, 1500, 1000);

    TEST_ASSERT_FALSE_MESSAGE(ctx.pyro1_fired, "the flight record claims a deployment that did not happen");
    TEST_ASSERT_NULL_MESSAGE(strstr(mock_uart_buf, "$PYRO_FIRE"), "and so does the ground station");
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.refused_count, "a refusal is asked once, not every tick");
}

/* A board that fires the drogue and then refuses the retry: the retry is
 * spent, and the refusal is logged once rather than every tick. */
void test_REV03_refused_retry_is_asked_once(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro2_mode = PYRO_MODE_NONE;
    ctx.config.landing_timeout = 0;
    ctx.current_state = FALLING;
    ctx.apogee_detected = true;
    ctx.pyros_armed = true;
    ctx.pyro1_fired = true;
    ctx.pyro1_verify_fail = true; /* the charge did not light */
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    pp_test_prime(101325);
    pp_ground_track(false);
    ctx.last_altitude = 50000;
    mock_time_ms = 0;
    ctx.pyro1_fire_time = 1;
    mock_pyro.refuse_fire = true;

    descend_steady(&ctx, 50000, 4000, 4000); /* past the 2 s grace; faster than any canopy */

    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.refused_count, "the refused retry must be asked once");
    TEST_ASSERT_EQUAL(1, ctx.pyro1_refires);
    TEST_ASSERT_TRUE(ctx.pyro1_refused);
}

/* [CFG-SUBSYS-01, REV-12] telem_rate_hz is the in-flight cadence. */
void test_REV12_telem_rate_hz_sets_the_flight_cadence(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.telem_rate_hz = 5;
    ctx.current_state = ASCENT;
    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    for (uint32_t t = 1; t <= 2000; t++) {
        flight_update_outputs(&ctx, 10000 + t);
    }
    TEST_ASSERT_EQUAL_MESSAGE(10, count_pyro_sentences(), "5 Hz for two seconds is ten sentences");
}

/* ── On USB [USB-01..04] ──────────────────────────────────────────
 *
 * A board on USB is on a bench: it must not detect a launch or say a status
 * code. Attaching plays one double chirp; detaching gives it all back. */

/* Drive the buzzer for ms, counting tones. */
static void buzz_for(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        mock_time_ms++;
        hal_tasks_tick(mock_time_ms);
    }
}

void test_USB_01_no_launch_while_attached(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;
    pad_run(&ctx, 1000);
    flight_set_usb_attached(&ctx, true, mock_time_ms);

    /* 900 Pa is far past 100 ft, and rising fast. */
    uint32_t end = mock_time_ms + 3000;
    while (mock_time_ms < end) {
        mock_time_ms += 20;
        mock_pressure.pressure_pa = 101325.0f - 900.0f;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "a board on USB detected a launch");

    flight_set_usb_attached(&ctx, false, mock_time_ms);
    pp_test_prime(101325);
    pad_run(&ctx, 1000);
    end = mock_time_ms + 3000;
    while (mock_time_ms < end && ctx.current_state == PAD_IDLE) {
        mock_time_ms += 20;
        mock_pressure.pressure_pa = 101325.0f - 900.0f;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL_MESSAGE(ASCENT, ctx.current_state, "unplugged, the same climb must be a launch");
}

void test_USB_02_attach_silences_the_pad_and_chirps_once(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    buzzer_init();
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);
    flight_set_usb_attached(&ctx, true, mock_time_ms);
    buzz_for(2000);
    TEST_ASSERT_EQUAL_MESSAGE(10, mock_buzzer_tone_on_count, "attach is one double chirp: two bursts of five");

    int after_chirp = mock_buzzer_tone_on_count;
    pad_run(&ctx, 20000);
    buzz_for(100);
    TEST_ASSERT_EQUAL_MESSAGE(after_chirp, mock_buzzer_tone_on_count, "a board on USB said a status code");
    TEST_ASSERT_FALSE(ctx.buzzer_started);

    flight_set_usb_attached(&ctx, false, mock_time_ms);
    pad_run(&ctx, 2500);
    TEST_ASSERT_TRUE_MESSAGE(ctx.buzzer_started, "unplugged, the pad outcome must be announced again");
    TEST_ASSERT_EQUAL(BR_OK_TO_FLY, ctx.last_reason);
    TEST_ASSERT_GREATER_THAN(after_chirp, mock_buzzer_tone_on_count);
}

void test_USB_03_attach_silences_an_announcement_in_progress(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    buzzer_init();
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);
    pad_run(&ctx, 1500);
    TEST_ASSERT_TRUE(ctx.buzzer_started); /* OK to fly, until launch */

    flight_set_usb_attached(&ctx, true, mock_time_ms);
    buzz_for(2000); /* the double chirp */
    int quiet = mock_buzzer_tone_on_count;
    pad_run(&ctx, 15000);
    buzz_for(100);
    TEST_ASSERT_EQUAL_MESSAGE(quiet, mock_buzzer_tone_on_count, "the pad announcement kept going on USB");
}

void test_USB_04_landed_beepout_stops_and_resumes(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    buzzer_init();
    ctx.current_state = LANDED;
    ctx.max_altitude = 25100; /* 251 m */
    buzzer_play_altitude(251);
    buzz_for(3000);
    TEST_ASSERT_TRUE(buzzer_is_active());

    flight_set_usb_attached(&ctx, true, mock_time_ms);
    buzz_for(2000);
    int quiet = mock_buzzer_tone_on_count;
    buzz_for(20000);
    TEST_ASSERT_EQUAL_MESSAGE(quiet, mock_buzzer_tone_on_count, "the altitude beep-out kept going on USB");

    flight_set_usb_attached(&ctx, false, mock_time_ms);
    buzz_for(20000);
    TEST_ASSERT_GREATER_THAN_MESSAGE(quiet, mock_buzzer_tone_on_count, "unplugged, the beep-out must resume");
}

void test_USB_05_fault_announcement_stops_and_resumes(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    buzzer_init();
    ctx.current_state = BOOT_SETTLE;
    ctx.sensor_type = 0; /* no sensor: FAULT */
    mock_time_ms = 2600;
    ctx.current_state = step(&ctx, mock_time_ms);
    ctx.current_state = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL(FAULT, ctx.current_state);
    buzz_for(3000);
    TEST_ASSERT_TRUE(buzzer_is_active());

    flight_set_usb_attached(&ctx, true, mock_time_ms);
    buzz_for(2000);
    int quiet = mock_buzzer_tone_on_count;
    buzz_for(20000);
    TEST_ASSERT_EQUAL_MESSAGE(quiet, mock_buzzer_tone_on_count, "the fault announcement kept going on USB");

    flight_set_usb_attached(&ctx, false, mock_time_ms);
    buzz_for(10000);
    TEST_ASSERT_GREATER_THAN_MESSAGE(quiet, mock_buzzer_tone_on_count, "unplugged, the fault must be announced");
}

/* Once airborne the flag changes nothing: a cable cannot be attached in
 * flight, and a flight must never be abandoned on the strength of one. */
void test_USB_06_ignored_once_airborne(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_DELAY;
    ctx.config.pyro1_value = 0;
    ctx.config.pyro2_mode = PYRO_MODE_NONE;
    ctx.config.landing_timeout = 0;
    ctx.current_state = FALLING;
    ctx.apogee_detected = true;
    ctx.pyros_armed = true;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;
    pp_test_prime(101325);
    pp_ground_track(false);
    ctx.last_altitude = 50000;
    mock_time_ms = 0;
    ctx.apogee_time = 0;
    buzzer_init();
    flight_set_usb_attached(&ctx, true, mock_time_ms);
    descend_steady(&ctx, 50000, 1500, 500);
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyro1_fired, "the flag stopped a flight in progress");
    TEST_ASSERT_EQUAL_MESSAGE(0, mock_buzzer_tone_on_count, "a board in flight chirped");
}

/* ── Test mode [USB-08] ───────────────────────────────────────────
 *
 * A chamber flight or a bench soak needs the flight machine with a PC on the
 * port. Test mode gives it back: the board behaves as it does on battery. */

void test_USB_07_test_mode_flies_on_usb(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = PAD_IDLE;
    pp_test_prime(101325);
    mock_time_ms = 0;
    pad_run(&ctx, 1000);
    flight_set_usb_attached(&ctx, true, mock_time_ms);
    flight_set_test_mode(&ctx, true, mock_time_ms);

    uint32_t end = mock_time_ms + 3000;
    while (mock_time_ms < end && ctx.current_state == PAD_IDLE) {
        mock_time_ms += 20;
        mock_pressure.pressure_pa = 101325.0f - 900.0f;
        ctx.current_state = step(&ctx, mock_time_ms);
    }
    TEST_ASSERT_EQUAL_MESSAGE(ASCENT, ctx.current_state, "test mode on USB must detect the launch");
}

void test_USB_08_test_mode_announces_and_leaving_it_chirps(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    buzzer_init();
    mock_pressure.pressure_pa = 101325.0f;
    boot_to_pad_idle(&ctx);
    flight_set_usb_attached(&ctx, true, mock_time_ms);
    buzz_for(2000);
    int after_chirp = mock_buzzer_tone_on_count;

    flight_set_test_mode(&ctx, true, mock_time_ms);
    pad_run(&ctx, 2500);
    TEST_ASSERT_TRUE_MESSAGE(ctx.buzzer_started, "test mode on USB must announce the pad verdict");
    TEST_ASSERT_GREATER_THAN(after_chirp, mock_buzzer_tone_on_count);

    flight_set_test_mode(&ctx, false, mock_time_ms);
    int before = mock_buzzer_tone_on_count;
    buzz_for(2000);
    TEST_ASSERT_EQUAL_MESSAGE(before + 10, mock_buzzer_tone_on_count, "leaving test mode on USB is an attach");
    before = mock_buzzer_tone_on_count;
    pad_run(&ctx, 15000);
    buzz_for(100);
    TEST_ASSERT_EQUAL_MESSAGE(before, mock_buzzer_tone_on_count, "and the board is quiet again");
}

/* Held in RAM: a board is never plugged in to find itself still in test mode
 * from a session someone forgot about. */
void test_USB_09_test_mode_is_off_at_boot(void) {
    static flight_context_t ctx;
    flight_init(&ctx);
    ctx.current_state = PAD_IDLE;
    flight_set_test_mode(&ctx, true, 0);
    TEST_ASSERT_TRUE(ctx.test_mode);
    flight_init(&ctx);
    TEST_ASSERT_FALSE_MESSAGE(ctx.test_mode, "test mode survived a boot");
}

void test_USB_10_test_mode_does_not_change_in_flight(void) {
    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.current_state = FALLING;
    flight_set_test_mode(&ctx, true, 0);
    TEST_ASSERT_FALSE(ctx.test_mode);
    ctx.current_state = PAD_IDLE;
    flight_set_test_mode(&ctx, true, 0);
    ctx.current_state = ASCENT;
    flight_set_test_mode(&ctx, false, 0);
    TEST_ASSERT_TRUE(ctx.test_mode);
}

/* [PYR-SAFE-04, REV-18] The HTTP interlock's question: is the rocket flying?
 * True from launch to landing and at no other time -- a board in FAULT or
 * LANDED must still take a reboot or a firmware image. */
void test_REV18_flight_in_progress_is_launch_to_landing(void) {
    static flight_context_t ctx;
    flight_init(&ctx);
    static const struct {
        flight_state_t st;
        bool flying;
    } cases[] = {
        {BOOT_SETTLE, false},  {BOOT_SENSOR, false}, {BOOT_CONTINUITY, false}, {BOOT_CALIBRATE, false},
        {PAD_IDLE, false},     {ASCENT, true},       {FALLING, true},          {DROGUE_DESCENT, true},
        {CHUTE_DESCENT, true}, {LANDED, false},      {FAULT, false},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ctx.current_state = cases[i].st;
        char m[48];
        snprintf(m, sizeof(m), "state %d", (int)cases[i].st);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].flying, flight_in_progress(), m);
    }
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
    RUN_TEST(test_FLT_BOOT_13_no_calibration_samples_is_fault);
    RUN_TEST(test_FLT_BOOT_14_no_filesystem_is_fault);
    RUN_TEST(test_FLT_BOOT_02_reads_config_at_boot);
    RUN_TEST(test_FLT_BOOT_03_writes_default_config);

    /* PAD_IDLE */
    RUN_TEST(test_FLT_LAUNCH_02_stays_on_ground);
    RUN_TEST(test_FLT_LAUNCH_01_detects_ascent);
    RUN_TEST(test_GND_CAL_01_reference_follows_slow_drift);
    RUN_TEST(test_GND_CAL_02_reference_stops_tracking_when_the_rocket_moves);
    RUN_TEST(test_FLT_LAUNCH_08_ten_metres_is_no_longer_enough);
    RUN_TEST(test_FLT_LAUNCH_09_freezing_keeps_the_hundred_feet);
    RUN_TEST(test_PYR_CONT_01_continuity_check);

    /* ASCENT */
    RUN_TEST(test_FLT_ASC_01_tracks_max_altitude);
    RUN_TEST(test_FLT_ASC_04_arms_pyros);
    RUN_TEST(test_FLT_ASC_07_arms_after_ten_metres_a_second);
    RUN_TEST(test_FLT_APO_01_detects_apogee);

    /* FALLING / DROGUE / CHUTE */
    RUN_TEST(test_FLT_DESC_03_drogue_phase_from_rate);
    RUN_TEST(test_FLT_DESC_04_chute_phase_from_rate);
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

    /* Code review 2026-09-24 */
    RUN_TEST(test_REV04_pad_fault_after_boot_is_announced);
    RUN_TEST(test_REV_NEW_disabled_channel_is_not_a_fault);
    RUN_TEST(test_REV07_launch_backdates_to_first_rise);
    RUN_TEST(test_REV08_fault_sends_no_state_sentence);
    RUN_TEST(test_REV09_flight_time_freezes_at_landing);
    RUN_TEST(test_REV03_refused_fire_is_not_recorded_as_fired);
    RUN_TEST(test_REV03_refused_retry_is_asked_once);
    RUN_TEST(test_REV12_telem_rate_hz_sets_the_flight_cadence);
    RUN_TEST(test_REV18_flight_in_progress_is_launch_to_landing);

    /* On USB */
    RUN_TEST(test_USB_01_no_launch_while_attached);
    RUN_TEST(test_USB_02_attach_silences_the_pad_and_chirps_once);
    RUN_TEST(test_USB_03_attach_silences_an_announcement_in_progress);
    RUN_TEST(test_USB_04_landed_beepout_stops_and_resumes);
    RUN_TEST(test_USB_05_fault_announcement_stops_and_resumes);
    RUN_TEST(test_USB_06_ignored_once_airborne);
    RUN_TEST(test_USB_07_test_mode_flies_on_usb);
    RUN_TEST(test_USB_08_test_mode_announces_and_leaving_it_chirps);
    RUN_TEST(test_USB_09_test_mode_is_off_at_boot);
    RUN_TEST(test_USB_10_test_mode_does_not_change_in_flight);

    return UNITY_END();
}
