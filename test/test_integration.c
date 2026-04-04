/*
 * System integration test: simulate a complete flight using OpenRocket
 * trajectory data with interpolated pressure at 1ms resolution.
 *
 * The mock pressure sensor interpolates between OpenRocket data points
 * to provide smooth pressure data at any sample time. The full state
 * machine runs at real timing (10ms PAD_IDLE, 100ms ASCENT, etc.)
 * with tud_task/net_service simulated as no-ops.
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/flight_states.h"
#include "pressure_processing.h"
#include "../src/telemetry_formatter.h"
#include "../src/hal.h"
#include "../src/buzzer.h"

/* Wrapper: feed pressure into pp, then dispatch */
static flight_state_t step(flight_context_t *ctx, uint32_t now) {
    hal_tasks_tick(now);
    return dispatch_state(ctx, now);
}

/* ── OpenRocket data ──────────────────────────────────────────────── */

#define MAX_POINTS 300

typedef struct {
    float time_s;
    float altitude_ft;
} sim_point_t;

static sim_point_t sim_data[MAX_POINTS];
static int sim_count = 0;

static void load_sim_data(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Cannot open %s\n", path);
        return;
    }
    char line[256];
    sim_count = 0;
    while (fgets(line, sizeof(line), f) && sim_count < MAX_POINTS) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        float t, alt;
        if (sscanf(line, "%f,%f", &t, &alt) == 2) {
            sim_data[sim_count].time_s = t;
            sim_data[sim_count].altitude_ft = alt;
            sim_count++;
        }
    }
    fclose(f);
}

/* Interpolate altitude at any time from sim data */
static float interpolate_altitude_ft(float time_s) {
    if (sim_count == 0)
        return 0.0f;
    if (time_s <= sim_data[0].time_s)
        return sim_data[0].altitude_ft;
    if (time_s >= sim_data[sim_count - 1].time_s)
        return sim_data[sim_count - 1].altitude_ft;

    /* Binary search for bracket */
    int lo = 0, hi = sim_count - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (sim_data[mid].time_s <= time_s)
            lo = mid;
        else
            hi = mid;
    }

    /* Linear interpolation */
    float t0 = sim_data[lo].time_s, t1 = sim_data[hi].time_s;
    float a0 = sim_data[lo].altitude_ft, a1 = sim_data[hi].altitude_ft;
    float frac = (time_s - t0) / (t1 - t0);
    return a0 + frac * (a1 - a0);
}

/* Convert altitude (ft) to pressure (Pa)
 * Inverse of: altitude_cm = (ground_pa - pressure) * 83 / 10
 * So: pressure = ground_pa - altitude_cm * 10 / 83
 */
static float altitude_ft_to_pressure_pa(float alt_ft, float ground_pa) {
    float alt_cm = alt_ft * 30.48f;
    return ground_pa - (alt_cm * 10.0f / 83.0f);
}

/* Update mock pressure from interpolated sim data */
static void update_mock_pressure(uint32_t now_ms) {
    float time_s = (float)now_ms / 1000.0f;
    float alt_ft = interpolate_altitude_ft(time_s);
    mock_pressure.pressure_pa = altitude_ft_to_pressure_pa(alt_ft, 101325.0f);
}

/* ── Buzzer mock ──────────────────────────────────────────────────── */
/* The integration test does not compile src/buzzer.c, so we provide
 * lightweight stubs here.  buzzer_set_code/set_altitude are now static
 * inline shims in buzzer.h that call buzzer_play_code/play_altitude,
 * so we only need to define the real functions. */

static int buzzer_code_count = 0;
static uint8_t last_buzzer_code = 0;
static int buzzer_stop_count = 0;
static int buzzer_altitude_count = 0;
static int32_t last_buzzer_altitude = 0;
static bool buzzer_active_flag = false;

void buzzer_init(void) {}
void buzzer_play_code(uint8_t code, uint8_t repeat_count) {
    (void)repeat_count;
    buzzer_code_count++;
    last_buzzer_code = code;
    buzzer_active_flag = true;
}
void buzzer_play_altitude(int32_t altitude) {
    buzzer_altitude_count++;
    last_buzzer_altitude = altitude;
    buzzer_active_flag = true;
}
void buzzer_stop(void) {
    buzzer_stop_count++;
    buzzer_active_flag = false;
}
bool buzzer_is_active(void) {
    return buzzer_active_flag;
}

/* ── Simulation runner ────────────────────────────────────────────── */

static flight_context_t ctx;

static void reset_sim(void) {
    mock_reset_all();
    pp_init();
    pp_test_prime(101325); /* skip boot calibration — start in PP_RUNNING */
    memset(&ctx, 0, sizeof(ctx));
    /* pyro1: delay 0s (fire at apogee), pyro2: AGL 50ft, units=ft */
    ctx.config = (config_t){.id = "SIM",
                            .name = "SIM",
                            .pyro1_mode = PYRO_MODE_DELAY,
                            .pyro1_value = 0,
                            .pyro2_mode = PYRO_MODE_AGL,
                            .pyro2_value = 50,
                            .units = 2};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    buzzer_code_count = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true; /* startup beep active */
    mock_uart_len = 0;
    telemetry_init(&ctx.config); /* formatter must know format before first send */
}

/* Run the full application loop for one millisecond tick */
static void app_tick(uint32_t now_ms) {
    mock_time_ms = now_ms;
    update_mock_pressure(now_ms);
    mock_pyro.firing = false;

    /* This is what main() does each iteration */
    ctx.current_state = step(&ctx, now_ms);

    /* Telemetry + buzzer + pyro update via flight_update_outputs()
     * (same call path as real firmware main_hardware.c). */
    flight_update_outputs(&ctx, now_ms);
}

/* Run full simulation from t=0 to end of sim data + 2s settling */
static void run_full_sim(void) {
    float end_time_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_time_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
    }
}

/* ── Tests ────────────────────────────────────────────────────────── */

void setUp(void) {
    pp_init();
}
void tearDown(void) {}

void test_TST_02_sim_data_loads(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    TEST_ASSERT_TRUE(sim_count > 100);
}

void test_TST_02_interpolation(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    /* t=0 should be 0 altitude */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, interpolate_altitude_ft(0.0f));
    /* Mid-flight should be positive */
    TEST_ASSERT_TRUE(interpolate_altitude_ft(3.0f) > 100.0f);
    /* After landing should be ~0 */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, interpolate_altitude_ft(20.0f));
}

void test_SNS_ALT_01_roundtrip(void) {
    /* 100 ft -> pressure -> altitude should round-trip */
    float pa = altitude_ft_to_pressure_pa(100.0f, 101325.0f);
    int32_t alt_cm = pp_pressure_to_altitude_cm((int32_t)pa, 101325);
    /* 100 ft = 3048 cm */
    TEST_ASSERT_INT_WITHIN(50, 3048, alt_cm);
}

void test_FLT_BOOT_01_all_states(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    bool saw_pad = false, saw_ascent = false, saw_descent = false, saw_landed = false;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == PAD_IDLE)
            saw_pad = true;
        if (ctx.current_state == ASCENT)
            saw_ascent = true;
        if (ctx.current_state == DESCENT)
            saw_descent = true;
        if (ctx.current_state == LANDED)
            saw_landed = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_pad, "Never in PAD_IDLE");
    TEST_ASSERT_TRUE_MESSAGE(saw_ascent, "Never in ASCENT");
    TEST_ASSERT_TRUE_MESSAGE(saw_descent, "Never in DESCENT");
    TEST_ASSERT_TRUE_MESSAGE(saw_landed, "Never in LANDED");
}

void test_FLT_APO_01_detected(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE(ctx.apogee_detected);
    /* Peak ~165 ft = ~5029 cm, allow filter lag */
    TEST_ASSERT_INT_WITHIN(2000, 5000, ctx.max_altitude);
}

void test_PYR_MODE_01_fires(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(mock_pyro.fire_count > 0, "No pyro fired");
}

void test_BUZ_07_03_lifecycle(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(buzzer_stop_count > 0, "Buzzer never stopped (launch)");
    TEST_ASSERT_TRUE_MESSAGE(buzzer_altitude_count > 0, "No altitude beep-out (landing)");
    TEST_ASSERT_TRUE(last_buzzer_altitude > 0);
}

void test_DAT_04_events(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    /* hal_log_stop() was called at landing; flight_log.csv is complete.
     * The incremental ring-buffer CSV logger is retired (v2-9). */
    char buf[32768];
    int n = hal_fs_read_file("flight_log.csv", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "LAUNCH") != NULL, "Expected LAUNCH event");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "LANDING") != NULL, "Expected LANDING event");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "PYRO1") != NULL || strstr(buf, "PYRO2") != NULL, "Expected pyro event");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "ARMED") != NULL, "Expected ARMED event");
}

/* [TEL-03] NMEA event sentences are emitted at apogee, fire, and landing */
void test_TEL_03_event_sentences(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "$PYRO_APO,") != NULL, "Missing $PYRO_APO event sentence");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "$PYRO_FIRE,") != NULL, "Missing $PYRO_FIRE event sentence");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "$PYRO_LAND,") != NULL, "Missing $PYRO_LAND event sentence");
}

/* [TEL-04] JSON format (telem_format=1) emits JSON objects; no NMEA sentences */
void test_TEL_04_json_format(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    /* Switch to JSON before running — s_cfg pointer already points to ctx.config */
    ctx.config.telem_format = TELEM_FORMAT_JSON;
    telemetry_init(&ctx.config);
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "{\"t\":\"state\"") != NULL, "Missing JSON state objects");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "{\"t\":\"apogee\"") != NULL, "Missing JSON apogee event");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "{\"t\":\"fire\"") != NULL, "Missing JSON fire event");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "{\"t\":\"landing\"") != NULL, "Missing JSON landing event");
    TEST_ASSERT_NULL_MESSAGE(strstr(mock_uart_buf, "$PYRO"), "NMEA sentences present in JSON mode");
}

void test_TEL_01_output(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE(mock_uart_len > 0);

    /* Count $PYRO sentences */
    int count = 0;
    char *p = mock_uart_buf;
    while ((p = strstr(p, "$PYRO,")) != NULL) {
        count++;
        p++;
    }
    TEST_ASSERT_TRUE_MESSAGE(count >= 10, "Expected >=10 telemetry sentences");

    /* Verify checksum on first sentence */
    char *dollar = strchr(mock_uart_buf, '$');
    char *star = strchr(mock_uart_buf, '*');
    TEST_ASSERT_NOT_NULL(dollar);
    TEST_ASSERT_NOT_NULL(star);
    uint8_t expected = 0;
    for (char *c = dollar + 1; c < star; c++)
        expected ^= (uint8_t)*c;
    unsigned int actual;
    sscanf(star + 1, "%02X", &actual);
    TEST_ASSERT_EQUAL_HEX8(expected, (uint8_t)actual);
}

void test_FLT_LAUNCH_01_timing(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    uint32_t ascent_start = 0, descent_start = 0, landed_start = 0;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == ASCENT && ascent_start == 0)
            ascent_start = t;
        if (ctx.current_state == DESCENT && descent_start == 0)
            descent_start = t;
        if (ctx.current_state == LANDED && landed_start == 0)
            landed_start = t;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Ascent=%u Descent=%u Landed=%u", ascent_start, descent_start, landed_start);
    TEST_ASSERT_TRUE_MESSAGE(ascent_start > 0 && ascent_start < 3000, msg);
    TEST_ASSERT_TRUE_MESSAGE(descent_start > 2000 && descent_start < 6000, msg);
    TEST_ASSERT_TRUE_MESSAGE(landed_start > 12000, msg);
}

void test_FLT_LAND_04_duration(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    /* Flight time should be ~15 seconds */
    TEST_ASSERT_TRUE(ctx.launch_time > 0);
    uint32_t flight_ms = sim_data[sim_count - 1].time_s * 1000 - ctx.launch_time;
    TEST_ASSERT_INT_WITHIN(5000, 15000, flight_ms);
}

void test_DAT_06_csv_export(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    /* Use batch flight_save_csv() to test the complete header format
     * (includes Max Alt which is unknown at incremental header time).
     * Event completeness (LAUNCH etc.) is tested in test_DAT_04_events. */
    flight_save_csv(&ctx);

    char buf[4096];
    int n = hal_fs_read_file("flight.csv", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "CSV file not written");
    buf[n] = '\0';

    /* Header must contain all metadata fields without truncation */
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Pyro MK1B") != NULL, "Missing header start");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# ID:") != NULL, "Missing ID");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Pyro1:") != NULL, "Missing Pyro1 config");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Pyro2:") != NULL, "Missing Pyro2 config");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Max Alt cm:") != NULL, "Missing max altitude");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "time_ms,pressure_pa,altitude_cm,state,thrust,event") != NULL,
                             "Missing CSV columns");

    /* Must contain data rows (last 64 ring buffer entries) */
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "LANDING") != NULL, "Missing LANDING in batch CSV");
}

/* [FLT-LAUNCH-03] Launch time is backdated toward the first sample above 50cm
 * AGL by walking the ring buffer.  The buffer is 64 entries × 10ms = 640ms
 * deep.  In this OpenRocket CSV the rocket crosses 50cm at ~210ms and launch
 * is not detected until ~1060ms (850ms later), so the 50cm crossing is outside
 * the buffer window and no backdate is applied.  The invariant is therefore:
 *   ctx.launch_time <= ascent_start_ms
 * (equality when the crossing is beyond buffer depth; earlier when it fits). */
void test_FLT_LAUNCH_03_backdate(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    uint32_t ascent_start_ms = 0;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == ASCENT && ascent_start_ms == 0)
            ascent_start_ms = t;
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.launch_time > 0, "Launch time never set");
    TEST_ASSERT_TRUE_MESSAGE(ascent_start_ms > 0, "Never reached ASCENT");
    char msg[128];
    snprintf(msg, sizeof(msg), "launch_time=%u > ascent_start=%u (must not be set after ASCENT transition)",
             ctx.launch_time, ascent_start_ms);
    TEST_ASSERT_TRUE_MESSAGE(ctx.launch_time <= ascent_start_ms, msg);
}

/* [FLT-APO-04] Apogee must never be detected before pyros are armed.
 * detect_ascent() gates SEVT_APOGEE on ctx->pyros_armed, so the ARMED
 * transition must precede the APOGEE transition. Verified two ways:
 * tick-by-tick flag inspection and armed_time < apogee_time ordering. */
void test_FLT_APO_04_no_apogee_before_armed(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    bool apogee_before_armed = false;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.apogee_detected && !ctx.pyros_armed)
            apogee_before_armed = true;
    }

    TEST_ASSERT_FALSE_MESSAGE(apogee_before_armed, "apogee_detected became true before pyros_armed");
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyros_armed, "Pyros never armed — check flight profile");
    TEST_ASSERT_TRUE_MESSAGE(ctx.apogee_detected, "Apogee never detected — check flight profile");
    char msg[128];
    snprintf(msg, sizeof(msg), "armed_time=%u >= apogee_time=%u", ctx.armed_time, ctx.apogee_time);
    TEST_ASSERT_TRUE_MESSAGE(ctx.armed_time < ctx.apogee_time, msg);
}

/* [FLT-ASC-03] ctx.under_thrust must be set while speed is increasing
 * during the ASCENT burn phase.
 * [FLT-ASC-06] Pyros must not arm while vertical_speed_cms > 1000 (10 m/s).
 * arming_gate_met() requires vertical_speed_cms < 1000 AND >= 0. */
void test_FLT_ASC_03_06_thrust_and_arming(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    bool saw_thrust = false;
    bool armed_while_fast = false;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        /* [FLT-ASC-03] under_thrust: set when speed increases tick-over-tick */
        if (ctx.current_state == ASCENT && ctx.under_thrust)
            saw_thrust = true;
        /* [FLT-ASC-06] arming gate must require speed < 10 m/s */
        if (ctx.pyros_armed && ctx.vertical_speed_cms > 1000)
            armed_while_fast = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_thrust, "under_thrust never set during ASCENT (FLT-ASC-03)");
    TEST_ASSERT_FALSE_MESSAGE(armed_while_fast, "Pyros armed while speed > 10 m/s (FLT-ASC-06)");
}

/* [PYR-ALT-02] BEEP_CFG_RANGE (4-3) must be emitted when any altitude-based
 * pyro setting exceeds the 8000m barometric ceiling.
 * update_continuity_and_buzzer() sets the beep code to BEEP_CFG_RANGE when
 * AGL/FALLEN/SPEED value > max_units (8000 in meters mode).
 *
 * [PYR-ALT-01] The clamp in should_fire_pyro() is implicitly exercised by
 * the Karman closed-loop suite: AGL thresholds exceeding 8000m are clamped
 * to max_units, and pyros still fire correctly on Karman altitude flights. */
void test_PYR_ALT_02_cfg_range_beep(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    /* Override to AGL 9000m (units=1=meters) — above the 8000m ceiling */
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_DELAY;
    ctx.config.pyro1_value = 0;
    ctx.config.pyro2_mode = PYRO_MODE_AGL;
    ctx.config.pyro2_value = 9000; /* 9000m > 8000m ceiling */
    ctx.config.units = 1;          /* meters */
    buzzer_code_count = 0;
    last_buzzer_code = 0;
    buzzer_active_flag = true;

    /* Run PAD_IDLE for 1500ms. update_continuity_and_buzzer() gates on
     * now - last_cont_check > 1000ms AND buzzer_started == false.
     * The buzzer code is set on the first continuity check after 1s. */
    for (uint32_t t = 0; t < 1500; t++) {
        mock_time_ms = t;
        update_mock_pressure(t);
        ctx.current_state = step(&ctx, t);
        if (ctx.current_state != PAD_IDLE)
            break;
    }

    TEST_ASSERT_TRUE_MESSAGE(buzzer_code_count > 0, "Buzzer code never set — update_continuity_and_buzzer not reached");
    char msg[64];
    snprintf(msg, sizeof(msg), "Expected BEEP_CFG_RANGE (0x%02X), got 0x%02X", BEEP_CFG_RANGE, last_buzzer_code);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BEEP_CFG_RANGE, last_buzzer_code, msg);
}

/* ── Ground test command tests [GND-TEST-01..04, DD-011] ──────────── */

/* Helper: set up a standalone PAD_IDLE context with good continuity
 * and advance time until update_continuity_and_buzzer() has fired at
 * least once (requires >1000ms to pass while in PAD_IDLE). */
static void setup_pad_idle_with_continuity(void) {
    reset_sim();
    ctx.filter_initialized = true;
    ctx.filtered_pressure = 101325;
    /* Advance to just past the 1000ms continuity-check gate so
     * last_status_code is set before we issue serial commands */
    for (uint32_t t = 0; t <= 1200; t += 10) {
        mock_time_ms = t;
        mock_pressure.pressure_pa = 101325.0f;
        ctx.current_state = step(&ctx, t);
        if (ctx.current_state != PAD_IDLE)
            break; /* should stay PAD_IDLE throughout */
    }
}

/* [GND-TEST-01] BEEP STATUS replays the last continuity status code */
void test_GND_TEST_01_beep_status_replay(void) {
    setup_pad_idle_with_continuity();

    /* Continuity check must have set last_status_code by now */
    TEST_ASSERT_TRUE_MESSAGE(ctx.last_status_code != 0, "last_status_code not set — continuity check not reached");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BEEP_ALL_GOOD, ctx.last_status_code, "Expected BEEP_ALL_GOOD for good continuity");

    int code_before = buzzer_code_count;
    mock_serial_enqueue("BEEP STATUS");
    mock_time_ms = 1210;
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, 1210);

    TEST_ASSERT_EQUAL_MESSAGE(code_before + 1, buzzer_code_count, "BEEP STATUS did not trigger buzzer_set_code()");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BEEP_ALL_GOOD, last_buzzer_code, "Replayed wrong beep code");
}

/* [GND-TEST-02] ARM then FIRE within the 3s window fires the pyro */
void test_GND_TEST_02_arm_fire_sequence(void) {
    setup_pad_idle_with_continuity();

    /* ARM channel 1 */
    mock_uart_len = 0;
    mock_serial_enqueue("ARM 1");
    mock_time_ms = 1210;
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, 1210);

    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "GT,ARMED,1") != NULL, "Expected $GT,ARMED,1 response");
    TEST_ASSERT_EQUAL_MESSAGE(GT_ARMED_1, ctx.gt.arm_state, "GT arm state should be GT_ARMED_1");

    /* FIRE channel 1 — 100ms later (well within 3s window) */
    int fire_before = mock_pyro.fire_count;
    mock_uart_len = 0;
    mock_serial_enqueue("FIRE 1");
    mock_time_ms = 1310;
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, 1310);

    TEST_ASSERT_EQUAL_MESSAGE(fire_before + 1, mock_pyro.fire_count, "Pyro 1 should fire after ARM 1 + FIRE 1");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "GT,FIRED,1") != NULL, "Expected $GT,FIRED,1 response");
    TEST_ASSERT_EQUAL_MESSAGE(GT_IDLE, ctx.gt.arm_state, "GT arm state should return to GT_IDLE after fire");
}

/* [GND-TEST-03] ARM auto-disarms after 3 seconds; FIRE then fails */
void test_GND_TEST_03_auto_disarm(void) {
    setup_pad_idle_with_continuity();

    /* ARM channel 1 at t=1210 */
    mock_serial_enqueue("ARM 1");
    mock_time_ms = 1210;
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, 1210);
    TEST_ASSERT_EQUAL(GT_ARMED_1, ctx.gt.arm_state);

    /* Advance 3001ms past the arm time — auto-disarm fires */
    mock_time_ms = 1210 + GT_ARM_TIMEOUT_MS + 10; /* = 4220ms */
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL_MESSAGE(GT_IDLE, ctx.gt.arm_state, "GT should auto-disarm after timeout");

    /* Now FIRE should fail — pyro must NOT fire */
    int fire_before = mock_pyro.fire_count;
    mock_uart_len = 0;
    mock_serial_enqueue("FIRE 1");
    mock_time_ms += 10;
    ctx.current_state = step(&ctx, mock_time_ms);
    TEST_ASSERT_EQUAL_MESSAGE(fire_before, mock_pyro.fire_count, "Pyro must NOT fire after auto-disarm timeout");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "GT,ERR") != NULL,
                             "Expected GT error response for fire after disarm");
}

/* [GND-TEST-04] Ground test commands are rejected outside PAD_IDLE */
void test_GND_TEST_04_only_in_pad_idle(void) {
    /* Directly call the handler with a context that is in ASCENT */
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    ctx.current_state = ASCENT;
    ctx.pyro1_continuity_good = true;
    ctx.pyro2_continuity_good = true;

    int fire_before = mock_pyro.fire_count;
    /* Pre-arm in the ground test context to test the state guard */
    ctx.gt.arm_state = GT_ARMED_1;
    ctx.gt.arm_time_ms = 0;

    /* Call handler directly with a FIRE command while in ASCENT */
    ground_test_handle_command(&ctx.gt, "FIRE 1", &ctx, 1000);

    TEST_ASSERT_EQUAL_MESSAGE(fire_before, mock_pyro.fire_count, "Pyro must NOT fire in ASCENT state");
    TEST_ASSERT_TRUE_MESSAGE(strstr(mock_uart_buf, "GT,ERR,not_pad_idle") != NULL,
                             "Expected GT,ERR,not_pad_idle when not in PAD_IDLE");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_TST_02_sim_data_loads);
    RUN_TEST(test_TST_02_interpolation);
    RUN_TEST(test_SNS_ALT_01_roundtrip);
    RUN_TEST(test_FLT_BOOT_01_all_states);
    RUN_TEST(test_FLT_APO_01_detected);
    RUN_TEST(test_PYR_MODE_01_fires);
    RUN_TEST(test_BUZ_07_03_lifecycle);
    RUN_TEST(test_DAT_04_events);
    RUN_TEST(test_TEL_03_event_sentences);
    RUN_TEST(test_TEL_04_json_format);
    RUN_TEST(test_TEL_01_output);
    RUN_TEST(test_FLT_LAUNCH_01_timing);
    RUN_TEST(test_FLT_LAND_04_duration);
    RUN_TEST(test_DAT_06_csv_export);
    RUN_TEST(test_FLT_LAUNCH_03_backdate);
    RUN_TEST(test_FLT_APO_04_no_apogee_before_armed);
    RUN_TEST(test_FLT_ASC_03_06_thrust_and_arming);
    RUN_TEST(test_PYR_ALT_02_cfg_range_beep);

    /* Ground test command tests [GND-TEST-01..04] */
    RUN_TEST(test_GND_TEST_01_beep_status_replay);
    RUN_TEST(test_GND_TEST_02_arm_fire_sequence);
    RUN_TEST(test_GND_TEST_03_auto_disarm);
    RUN_TEST(test_GND_TEST_04_only_in_pad_idle);

    return UNITY_END();
}
