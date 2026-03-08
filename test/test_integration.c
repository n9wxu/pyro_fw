/*
 * System integration test: feed OpenRocket simulation data through
 * the full state machine and verify flight behavior.
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/flight_states.h"
#include "../src/buzzer.h"

/* ── OpenRocket data loader ───────────────────────────────────────── */

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

/* Convert altitude (ft) to pressure (Pa) using inverse of our formula:
 * altitude_cm = (ground_pressure - pressure) * 83 / 10
 * pressure = ground_pressure - altitude_cm * 10 / 83
 */
static float altitude_ft_to_pressure_pa(float alt_ft, float ground_pa) {
    float alt_cm = alt_ft * 30.48f;
    return ground_pa - (alt_cm * 10.0f / 83.0f);
}

/* ── Buzzer mock tracking ─────────────────────────────────────────── */

static int buzzer_code_count = 0;
static uint8_t last_buzzer_code = 0;
static int buzzer_stop_count = 0;
static int buzzer_altitude_count = 0;
static int32_t last_buzzer_altitude = 0;

/* Override buzzer functions for tracking */
static bool buzzer_active_flag = false;

void buzzer_init(void) {}

void buzzer_set_code(uint8_t code, bool repeat) {
    (void)repeat;
    buzzer_code_count++;
    last_buzzer_code = code;
    buzzer_active_flag = true;
}

void buzzer_set_altitude(int32_t altitude) {
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

void buzzer_update(uint32_t now_ms) { (void)now_ms; }

/* ── Test helpers ─────────────────────────────────────────────────── */

static flight_context_t ctx;

static void reset_sim(void) {
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = (config_t){"SIM", "SIM", PYRO_MODE_DELAY, 0, PYRO_MODE_AGL, 50, 2, 0};
    /* pyro1: delay 0s (fire at apogee), pyro2: AGL 50ft, units=ft */
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = 101325;
    ctx.filter_initialized = false;
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    mock_pyro.firing = false;
    buzzer_code_count = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true; /* pretend startup beep is active */
    mock_uart_len = 0;
}

/* Run the state machine for one sim point */
static void sim_tick(float time_s, float alt_ft) {
    uint32_t now = (uint32_t)(time_s * 1000.0f);
    mock_time_ms = now;
    mock_pressure.pressure_pa = altitude_ft_to_pressure_pa(alt_ft, 101325.0f);
    mock_pyro.firing = false;
    ctx.current_state = dispatch_state(&ctx, now);

    /* Telemetry: 10Hz during ASCENT/DESCENT, 1Hz otherwise */
    if (ctx.current_state >= PAD_IDLE) {
        uint32_t interval = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
        if (now - ctx.last_telemetry >= interval) {
            uint32_t ft = (ctx.current_state != PAD_IDLE) ? (now - ctx.launch_time) : 0;
            send_telemetry(&ctx, ft, ctx.last_altitude, ctx.current_state);
            ctx.last_telemetry = now;
        }
    }
}

/* ── Integration tests ────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

void test_sim_data_loads(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    TEST_ASSERT_TRUE(sim_count > 100);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sim_data[0].altitude_ft);
}

void test_full_flight_state_sequence(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    bool saw_pad = false, saw_ascent = false, saw_descent = false, saw_landed = false;

    /* Run first continuity check to arm buzzer */
    ctx.last_cont_check = 0;
    sim_tick(0.0f, 0.0f);

    for (int i = 0; i < sim_count; i++) {
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

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

void test_apogee_detected(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    TEST_ASSERT_TRUE(ctx.apogee_detected);
    TEST_ASSERT_TRUE(ctx.max_altitude > 0);
}

void test_pyro1_fires_at_apogee(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    TEST_ASSERT_TRUE_MESSAGE(mock_pyro.fire_count > 0, "No pyro fired");
    /* Pyro 1 configured as delay=0 → fires at apogee */
    TEST_ASSERT_TRUE(ctx.pyro1_fired || mock_pyro.last_fire_channel == 1);
}

void test_buzzer_stops_on_launch(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++) {
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);
        if (ctx.current_state == ASCENT) break;
    }

    TEST_ASSERT_TRUE(buzzer_stop_count > 0);
}

void test_buzzer_altitude_on_landing(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    TEST_ASSERT_EQUAL(LANDED, ctx.current_state);
    TEST_ASSERT_TRUE(buzzer_altitude_count > 0);
    TEST_ASSERT_TRUE(last_buzzer_altitude > 0);
}

void test_data_log_has_samples(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    TEST_ASSERT_TRUE(ctx.buf_count > 50);
}

void test_data_log_has_events(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    /* Scan buffer for events */
    int launch_events = 0, apogee_events = 0, landing_events = 0, pyro_events = 0;
    for (int i = 0; i < ctx.buf_count; i++) {
        uint16_t idx = (ctx.buf_tail + i) % 4096;
        switch (ctx.flight_buffer[idx].event) {
            case EVT_LAUNCH:  launch_events++; break;
            case EVT_APOGEE:  apogee_events++; break;
            case EVT_LANDING: landing_events++; break;
            case EVT_PYRO1_FIRE: case EVT_PYRO2_FIRE: pyro_events++; break;
        }
    }

    TEST_ASSERT_EQUAL_MESSAGE(1, launch_events, "Expected 1 launch event");
    /* Apogee event may be overwritten by pyro fire on same sample */
    TEST_ASSERT_TRUE_MESSAGE(apogee_events + pyro_events >= 1, "Expected apogee or pyro event");
    TEST_ASSERT_EQUAL_MESSAGE(1, landing_events, "Expected 1 landing event");
    TEST_ASSERT_TRUE_MESSAGE(pyro_events > 0, "Expected pyro fire events");
}

void test_telemetry_emitted(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    /* UART should have $PYRO sentences */
    TEST_ASSERT_TRUE(mock_uart_len > 0);
    TEST_ASSERT_TRUE(strstr(mock_uart_buf, "$PYRO,") != NULL);

    /* Count sentences */
    int count = 0;
    char *p = mock_uart_buf;
    while ((p = strstr(p, "$PYRO,")) != NULL) { count++; p++; }
    TEST_ASSERT_TRUE_MESSAGE(count >= 5, "Expected at least 5 telemetry sentences");
}

void test_max_altitude_reasonable(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    for (int i = 0; i < sim_count; i++)
        sim_tick(sim_data[i].time_s, sim_data[i].altitude_ft);

    /* Peak is ~165 ft = ~5029 cm. Allow filter lag. */
    TEST_ASSERT_INT_WITHIN(2000, 5000, ctx.max_altitude);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_sim_data_loads);
    RUN_TEST(test_full_flight_state_sequence);
    RUN_TEST(test_apogee_detected);
    RUN_TEST(test_pyro1_fires_at_apogee);
    RUN_TEST(test_buzzer_stops_on_launch);
    RUN_TEST(test_buzzer_altitude_on_landing);
    RUN_TEST(test_data_log_has_samples);
    RUN_TEST(test_data_log_has_events);
    RUN_TEST(test_telemetry_emitted);
    RUN_TEST(test_max_altitude_reasonable);

    return UNITY_END();
}
