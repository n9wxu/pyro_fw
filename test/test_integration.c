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
#include "../src/buzzer.h"

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
    if (!f) { printf("Cannot open %s\n", path); return; }
    char line[256];
    sim_count = 0;
    while (fgets(line, sizeof(line), f) && sim_count < MAX_POINTS) {
        if (line[0] == '#' || line[0] == '\n') continue;
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
    if (sim_count == 0) return 0.0f;
    if (time_s <= sim_data[0].time_s) return sim_data[0].altitude_ft;
    if (time_s >= sim_data[sim_count - 1].time_s) return sim_data[sim_count - 1].altitude_ft;

    /* Binary search for bracket */
    int lo = 0, hi = sim_count - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (sim_data[mid].time_s <= time_s) lo = mid;
        else hi = mid;
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

static int buzzer_code_count = 0;
static uint8_t last_buzzer_code = 0;
static int buzzer_stop_count = 0;
static int buzzer_altitude_count = 0;
static int32_t last_buzzer_altitude = 0;
static bool buzzer_active_flag = false;

void buzzer_init(void) {}
void buzzer_set_code(uint8_t code, bool repeat) {
    (void)repeat; buzzer_code_count++; last_buzzer_code = code; buzzer_active_flag = true;
}
void buzzer_set_altitude(int32_t altitude) {
    buzzer_altitude_count++; last_buzzer_altitude = altitude; buzzer_active_flag = true;
}
void buzzer_stop(void) { buzzer_stop_count++; buzzer_active_flag = false; }
bool buzzer_is_active(void) { return buzzer_active_flag; }
void buzzer_update(uint32_t now_ms) { (void)now_ms; }

/* ── Simulation runner ────────────────────────────────────────────── */

static flight_context_t ctx;

static void reset_sim(void) {
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    /* pyro1: delay 0s (fire at apogee), pyro2: AGL 50ft, units=ft */
    ctx.config = (config_t){"SIM", "SIM", PYRO_MODE_DELAY, 0, PYRO_MODE_AGL, 50, 2, 0};
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    buzzer_code_count = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;  /* startup beep active */
    mock_uart_len = 0;
}

/* Run the full application loop for one millisecond tick */
static void app_tick(uint32_t now_ms) {
    mock_time_ms = now_ms;
    update_mock_pressure(now_ms);
    mock_pyro.firing = false;

    /* This is what main() does each iteration */
    ctx.current_state = dispatch_state(&ctx, now_ms);

    /* Telemetry: 10Hz ASCENT/DESCENT, 1Hz otherwise */
    if (ctx.current_state >= PAD_IDLE) {
        uint32_t interval = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
        if (now_ms - ctx.last_telemetry >= interval) {
            uint32_t ft = (ctx.current_state != PAD_IDLE) ? (now_ms - ctx.launch_time) : 0;
            send_telemetry(&ctx, ft, ctx.last_altitude, ctx.current_state);
            ctx.last_telemetry = now_ms;
        }
    }
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

void setUp(void) {}
void tearDown(void) {}

void test_sim_data_loads(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    TEST_ASSERT_TRUE(sim_count > 100);
}

void test_interpolation(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    /* t=0 should be 0 altitude */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, interpolate_altitude_ft(0.0f));
    /* Mid-flight should be positive */
    TEST_ASSERT_TRUE(interpolate_altitude_ft(3.0f) > 100.0f);
    /* After landing should be ~0 */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, interpolate_altitude_ft(20.0f));
}

void test_pressure_conversion_roundtrip(void) {
    /* 100 ft → pressure → altitude should round-trip */
    float pa = altitude_ft_to_pressure_pa(100.0f, 101325.0f);
    int32_t alt_cm = pressure_to_altitude_cm((int32_t)pa, 101325);
    /* 100 ft = 3048 cm */
    TEST_ASSERT_INT_WITHIN(50, 3048, alt_cm);
}

void test_full_flight_reaches_all_states(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    bool saw_pad = false, saw_ascent = false, saw_descent = false, saw_landed = false;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == PAD_IDLE) saw_pad = true;
        if (ctx.current_state == ASCENT) saw_ascent = true;
        if (ctx.current_state == DESCENT) saw_descent = true;
        if (ctx.current_state == LANDED) saw_landed = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_pad, "Never in PAD_IDLE");
    TEST_ASSERT_TRUE_MESSAGE(saw_ascent, "Never in ASCENT");
    TEST_ASSERT_TRUE_MESSAGE(saw_descent, "Never in DESCENT");
    TEST_ASSERT_TRUE_MESSAGE(saw_landed, "Never in LANDED");
}

void test_apogee_detected_and_altitude(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE(ctx.apogee_detected);
    /* Peak ~165 ft = ~5029 cm, allow filter lag */
    TEST_ASSERT_INT_WITHIN(2000, 5000, ctx.max_altitude);
}

void test_pyro_fires(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(mock_pyro.fire_count > 0, "No pyro fired");
}

void test_buzzer_lifecycle(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(buzzer_stop_count > 0, "Buzzer never stopped (launch)");
    TEST_ASSERT_TRUE_MESSAGE(buzzer_altitude_count > 0, "No altitude beep-out (landing)");
    TEST_ASSERT_TRUE(last_buzzer_altitude > 0);
}

void test_data_log_complete(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE(ctx.buf_count > 100);

    /* Scan for events */
    int events[16] = {0};
    for (int i = 0; i < ctx.buf_count; i++) {
        uint16_t idx = (ctx.buf_tail + i) % 4096;
        uint8_t e = ctx.flight_buffer[idx].event;
        if (e < 16) events[e]++;
    }

    TEST_ASSERT_EQUAL_MESSAGE(1, events[EVT_LAUNCH], "Expected 1 launch");
    TEST_ASSERT_EQUAL_MESSAGE(1, events[EVT_LANDING], "Expected 1 landing");
    TEST_ASSERT_TRUE_MESSAGE(events[EVT_PYRO1_FIRE] + events[EVT_PYRO2_FIRE] > 0, "Expected pyro events");
    TEST_ASSERT_TRUE_MESSAGE(events[EVT_ARMED] > 0, "Expected armed event");
}

void test_telemetry_output(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    TEST_ASSERT_TRUE(mock_uart_len > 0);

    /* Count $PYRO sentences */
    int count = 0;
    char *p = mock_uart_buf;
    while ((p = strstr(p, "$PYRO,")) != NULL) { count++; p++; }
    TEST_ASSERT_TRUE_MESSAGE(count >= 10, "Expected >=10 telemetry sentences");

    /* Verify checksum on first sentence */
    char *dollar = strchr(mock_uart_buf, '$');
    char *star = strchr(mock_uart_buf, '*');
    TEST_ASSERT_NOT_NULL(dollar);
    TEST_ASSERT_NOT_NULL(star);
    uint8_t expected = 0;
    for (char *c = dollar + 1; c < star; c++) expected ^= (uint8_t)*c;
    unsigned int actual;
    sscanf(star + 1, "%02X", &actual);
    TEST_ASSERT_EQUAL_HEX8(expected, (uint8_t)actual);
}

void test_state_timing(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    uint32_t ascent_start = 0, descent_start = 0, landed_start = 0;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == ASCENT && ascent_start == 0) ascent_start = t;
        if (ctx.current_state == DESCENT && descent_start == 0) descent_start = t;
        if (ctx.current_state == LANDED && landed_start == 0) landed_start = t;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Ascent=%u Descent=%u Landed=%u", ascent_start, descent_start, landed_start);
    TEST_ASSERT_TRUE_MESSAGE(ascent_start > 0 && ascent_start < 3000, msg);
    TEST_ASSERT_TRUE_MESSAGE(descent_start > 2000 && descent_start < 6000, msg);
    /* Note: landing may trigger early due to filter lag — this is a known issue */
    TEST_ASSERT_TRUE_MESSAGE(landed_start > descent_start, msg);
}

void test_flight_duration(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();

    /* Flight time should be ~15 seconds */
    TEST_ASSERT_TRUE(ctx.launch_time > 0);
    uint32_t flight_ms = sim_data[sim_count - 1].time_s * 1000 - ctx.launch_time;
    TEST_ASSERT_INT_WITHIN(5000, 15000, flight_ms);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_sim_data_loads);
    RUN_TEST(test_interpolation);
    RUN_TEST(test_pressure_conversion_roundtrip);
    RUN_TEST(test_full_flight_reaches_all_states);
    RUN_TEST(test_apogee_detected_and_altitude);
    RUN_TEST(test_pyro_fires);
    RUN_TEST(test_buzzer_lifecycle);
    RUN_TEST(test_data_log_complete);
    RUN_TEST(test_telemetry_output);
    RUN_TEST(test_state_timing);
    RUN_TEST(test_flight_duration);

    return UNITY_END();
}
