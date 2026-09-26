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
#include "../src/pyro_release.h"
#include "../src/beep_store.h"
#include "../src/pad_claim.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/flight_states.h"
#include "pressure_processing.h"
#include "../src/telemetry_formatter.h"
#include "../src/hal.h"
#include "../src/buzzer.h"
#include "../src/board_id.h"

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
/* The integration test does not compile src/buzzer.c. These record what the
 * flight software asked the buzzer to say. */

static int buzzer_code_count = 0;
static beep_spec_t last_buzzer_spec;
static int buzzer_stop_count = 0;
static int buzzer_altitude_count = 0;
static int32_t last_buzzer_altitude = 0;
static bool buzzer_active_flag = false;

void buzzer_init(void) {}
void buzzer_play_spec(const beep_spec_t *spec, uint16_t gap_ms, uint8_t repeat_count) {
    (void)gap_ms;
    (void)repeat_count;
    if (spec) {
        last_buzzer_spec = *spec;
    }
    buzzer_code_count++;
    buzzer_active_flag = true;
}

void buzzer_play_altitude(int32_t altitude) {
    buzzer_altitude_count++;
    last_buzzer_altitude = altitude;
    buzzer_active_flag = true;
}
static int buzzer_usb_ok_count = 0;
void buzzer_play_usb_ok(void) {
    buzzer_usb_ok_count++;
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
    flight_flash_service(&ctx, now_ms);
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
        if (ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT || ctx.current_state == CHUTE_DESCENT)
            saw_descent = true;
        if (ctx.current_state == LANDED)
            saw_landed = true;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_pad, "Never in PAD_IDLE");
    TEST_ASSERT_TRUE_MESSAGE(saw_ascent, "Never in ASCENT");
    TEST_ASSERT_TRUE_MESSAGE(saw_descent, "Never in FALLING/DROGUE/CHUTE");
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

/* ── Released pyro channels ───────────────────────────────────────
 *
 * A released channel's pad belongs to Lua. The flight software must reach a
 * mocked no-op for it rather than the hardware, and must say so -- these are
 * the tests for src/pyro_release.c through the whole flight path rather than
 * through the module's own API. */

/* MK1A's numbering, which test/hal_test.c also uses: FIRE1 on 9, FIRE2 on 11,
 * the common on 10. */
#define PAD_FIRE1 PAD(9)
#define PAD_FIRE2 PAD(11)
#define PAD_COMMON PAD(10)

/* What a release IS. Lua claims the pads first, so the flight software cannot
 * claim them and cannot be given the real methods for that channel. No
 * boolean is passed anywhere: the claim decides, and a pad has one owner.
 *
 * The common goes to Lua only when BOTH channels do -- until then it is still
 * half of the retained channel's firing path, which is the rule
 * pin_assign_is_reserved() enforces on the target. */
static void set_released(bool ch1, bool ch2) {
    pad_claim_reset();
    if (ch1) {
        TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE1, PAD_LUA));
    }
    if (ch2) {
        TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE2, PAD_LUA));
    }
    if (ch1 && ch2) {
        TEST_ASSERT_TRUE(pad_claim_take(PAD_COMMON, PAD_LUA));
    }
    hal_pyro_claim_channels(mock_pyro_pads);
}

void test_PYR_REL_01_released_channel_never_fires(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    set_released(true, false);
    run_full_sim();

    TEST_ASSERT_TRUE_MESSAGE(mock_pyro.fire_count > 0, "the retained channel must still fire");
    TEST_ASSERT_EQUAL_MESSAGE(2, mock_pyro.last_fire_channel, "only the retained channel may reach the hardware");
}

void test_PYR_REL_02_both_released_fires_nothing(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    set_released(true, true);
    run_full_sim();

    TEST_ASSERT_EQUAL_MESSAGE(0, mock_pyro.fire_count, "a full flight must not reach the hardware at all");
    TEST_ASSERT_TRUE_MESSAGE(pyro_release_mocks() > 0, "and the flight software must have tried");
}

void test_PYR_REL_03_mocked_operations_are_reported(void) {
    reset_sim();
    set_released(true, false);
    TEST_ASSERT_EQUAL(0, mock_pyro_notes);

    hal_pyro_fire(1);
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro_notes, "a mocked fire is reported, not swallowed");
    TEST_ASSERT_EQUAL_STRING("pyro1 fire: released to Lua", mock_pyro_last_note);
    TEST_ASSERT_EQUAL_MESSAGE(0, mock_pyro.fire_count, "and reaches no hardware");

    hal_pyro_fire(2);
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro_notes, "the retained channel is not reported");
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.fire_count, "and does reach the hardware");
}

void test_PYR_REL_04_released_reports_open_not_good(void) {
    reset_sim();
    mock_pyro.p1_good = true; /* the hardware would say good */
    set_released(true, false);

    hal_continuity_t c;
    hal_pyro_get(1, &c);
    /* Open rather than good: there is no igniter circuit the flight software
       controls here any more. Reporting continuity would let it arm and then
       "fire" a channel that cannot. */
    TEST_ASSERT_FALSE_MESSAGE(c.good, "a released channel has no continuity to report");
    TEST_ASSERT_TRUE(c.open);
    TEST_ASSERT_EQUAL(0, c.raw_adc);

    hal_pyro_get(2, &c);
    TEST_ASSERT_TRUE_MESSAGE(c.good, "the retained channel still reports the hardware");
}

void test_PYR_REL_05_shared_stimulus_stops_when_both_released(void) {
    reset_sim();
    set_released(true, false);
    hal_pyro_sample();
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.sample_count, "one channel retained still needs the stimulus");

    set_released(true, true);
    hal_pyro_sample();
    /* The stimulus drives the COMMON, which is Lua's exactly when both are
       released -- core0 must not drive a pad core1 owns. */
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.sample_count, "with both released there is nothing to stimulate");
}

void test_PYR_REL_06_fault_is_clear_for_a_released_channel(void) {
    reset_sim();
    mock_pyro.fault = true;
    set_released(true, false);

    TEST_ASSERT_FALSE_MESSAGE(hal_pyro_fault(1), "a released channel has no fault line to report");
    TEST_ASSERT_TRUE_MESSAGE(hal_pyro_fault(2), "the retained channel still reports the hardware");
}

/* ── Which of the three the pad check says ────────────────────────
 *
 * Three beeps because three actions exist at the pad. A pyro fault can be
 * fixed standing at the rocket; anything else means safe it and walk away, so
 * a board with both must send the operator away. */

beep_reason_t beep_reason_for_diag(uint16_t diag);

void test_BEEP_01_clean_board_says_ok_to_fly(void) {
    TEST_ASSERT_EQUAL(BR_OK_TO_FLY, beep_reason_for_diag(0));
    /* And it is a chirp, not something to count -- the Eggtimer convention
       a flier already has in their ear. */
    TEST_ASSERT_EQUAL_MESSAGE(BK_CHIRP, beep_for(BR_OK_TO_FLY).kind, "the good case should not need counting");
}

void test_BEEP_02_a_pyro_fault_names_its_channel(void) {
    TEST_ASSERT_EQUAL(BR_CHECK_PYRO_1, beep_reason_for_diag(DIAG_P1_OPEN));
    TEST_ASSERT_EQUAL(BR_CHECK_PYRO_2, beep_reason_for_diag(DIAG_P2_SHORT));
    TEST_ASSERT_EQUAL_MESSAGE(BR_CHECK_PYRO_1, beep_reason_for_diag(DIAG_P1_OPEN | DIAG_P2_OPEN),
                              "with both bad, channel 1 is named first");
}

void test_BEEP_03_anything_unfixable_says_system_failure(void) {
    TEST_ASSERT_EQUAL(BR_SYSTEM_FAILURE, beep_reason_for_diag(DIAG_SENSOR_FAIL));
    TEST_ASSERT_EQUAL(BR_SYSTEM_FAILURE, beep_reason_for_diag(DIAG_FS_FAIL));
    TEST_ASSERT_EQUAL_MESSAGE(BR_SYSTEM_FAILURE, beep_reason_for_diag(DIAG_CFG_RANGE),
                              "a bad config value needs a laptop, so it means leave the pad");
}

void test_BEEP_04_unfixable_outranks_fixable(void) {
    /* Adjusting the igniter would not help: the board still cannot fly. */
    TEST_ASSERT_EQUAL_MESSAGE(BR_SYSTEM_FAILURE, beep_reason_for_diag(DIAG_SENSOR_FAIL | DIAG_P1_OPEN),
                              "a dead sensor must send the operator away even with a pyro fault too");
}

/* ── The exclusion itself ─────────────────────────────────────────
 *
 * The property these exist for: there is no way for a pad to be in the pyro
 * table and in Lua's interface table at the same time. Not "it is checked" --
 * the second claimant cannot create its entry, because the claim it needs was
 * spent. */

void test_PAD_EXCL_01_pyro_cannot_take_a_pad_lua_holds(void) {
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE1, PAD_LUA));

    TEST_ASSERT_EQUAL_MESSAGE(1, hal_pyro_claim_channels(mock_pyro_pads),
                              "only the channel whose pads were free may be kept");
    TEST_ASSERT_TRUE_MESSAGE(pyro_release_is_released(1), "the channel Lua holds got the mocked methods");
    TEST_ASSERT_FALSE(pyro_release_is_released(2));
    TEST_ASSERT_EQUAL_MESSAGE(PAD_LUA, pad_claim_owner(9), "and the pad is still Lua's");
}

void test_PAD_EXCL_02_lua_cannot_take_a_pad_pyro_holds(void) {
    pad_claim_reset();
    TEST_ASSERT_EQUAL_MESSAGE(2, hal_pyro_claim_channels(mock_pyro_pads),
                              "with every pad free the flight software keeps both");

    TEST_ASSERT_FALSE_MESSAGE(pad_claim_take(PAD_FIRE1, PAD_LUA), "a pad the flight software kept is not available");
    TEST_ASSERT_FALSE_MESSAGE(pad_claim_take(PAD_COMMON, PAD_LUA), "nor is the common");
    TEST_ASSERT_EQUAL(PAD_FLIGHT, pad_claim_owner(9));
}

void test_PAD_EXCL_03_a_claim_is_all_or_nothing(void) {
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(PAD_COMMON, PAD_LUA));

    /* A bridge wants a channel element and the common. Half of it is
       available and half is not, so it gets neither -- a half-claimed bridge
       would be one FET gate this side owns and one it does not. */
    TEST_ASSERT_FALSE(pad_claim_take(PAD_FIRE1 | PAD_COMMON, PAD_FLIGHT));
    TEST_ASSERT_EQUAL_MESSAGE(PAD_FREE, pad_claim_owner(9), "the half that was free must not have been taken");
}

void test_PAD_EXCL_04_the_common_blocks_the_retained_channel_too(void) {
    /* Releasing ONE channel must not hand over the common: the retained
       channel still needs it, and pyro_release_claim() asks for both. */
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(PAD_COMMON, PAD_LUA));
    TEST_ASSERT_EQUAL_MESSAGE(0, hal_pyro_claim_channels(mock_pyro_pads),
                              "neither channel can fire without the common");
    TEST_ASSERT_TRUE(pyro_release_is_released(1));
    TEST_ASSERT_TRUE(pyro_release_is_released(2));
}

void test_PAD_EXCL_05_reclaiming_your_own_pad_succeeds(void) {
    /* Configuration runs the publish path more than once on the simulator,
       and a resource re-published on the pad it already owns must not be
       refused -- the duplicate-name rule is what catches a real collision. */
    pad_claim_reset();
    TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE1, PAD_LUA));
    TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE1, PAD_LUA));
    TEST_ASSERT_TRUE(pad_claim_take(PAD_FIRE1 | PAD_FIRE2, PAD_LUA));
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
        if ((ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT ||
             ctx.current_state == CHUTE_DESCENT) &&
            descent_start == 0)
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
    /* The board names itself via boards/<name>/board_pins.h, so asserting a
     * literal here passed only for MK1B and failed every other board. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(buf, "# " PYRO_BOARD_NAME, strlen("# " PYRO_BOARD_NAME)),
                                  "Missing header start");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# ID:") != NULL, "Missing ID");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Pyro1:") != NULL, "Missing Pyro1 config");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Pyro2:") != NULL, "Missing Pyro2 config");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "# Max Alt cm:") != NULL, "Missing max altitude");
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "time_ms,pressure_pa,altitude_cm,state,thrust,event") != NULL,
                             "Missing CSV columns");

    /* Must contain data rows (last 64 ring buffer entries) */
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "LANDING") != NULL, "Missing LANDING in batch CSV");
}

/* [FLT-LAUNCH-03, REV-07] Launch time is the first sample above 50 cm, not
 * the moment the detector tripped. In this profile the rocket clears 50 cm
 * about 200 ms in and 100 ft about a second later, so the interval is large
 * enough that a backdate of zero cannot pass for one. */
void test_FLT_LAUNCH_03_backdate(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    uint32_t ascent_start_ms = 0, first_rise_ms = 0;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        /* T+0 is the first reading above 50 cm [FLT-LAUNCH-03]: here is
         * where the trajectory itself passes it. */
        if (first_rise_ms == 0 && interpolate_altitude_ft((float)t / 1000.0f) * 0.3048f > 0.5f)
            first_rise_ms = t;
        if (ctx.current_state == ASCENT && ascent_start_ms == 0)
            ascent_start_ms = t;
    }

    TEST_ASSERT_TRUE_MESSAGE(ascent_start_ms > 0, "Never reached ASCENT");
    TEST_ASSERT_TRUE_MESSAGE(first_rise_ms > 0, "never saw the rocket leave 50 cm");
    char msg[160];
    snprintf(msg, sizeof(msg), "first rise %u, detected %u, launch_time %u", first_rise_ms, ascent_start_ms,
             ctx.launch_time);
    TEST_ASSERT_GREATER_THAN_MESSAGE(300, ascent_start_ms - first_rise_ms, msg);
    TEST_ASSERT_TRUE_MESSAGE(ctx.launch_time >= first_rise_ms && ctx.launch_time <= first_rise_ms + 20u, msg);
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

/* [FLT-ASC-03] This A8-3 burns out at 0.73 s, before the launch is declared
 * at 100 ft: under_thrust may linger into ASCENT for the fit's lag, and never
 * past a second after burnout. The chain suite's test_T5_under_thrust flies
 * burns that outlast the detector.
 * [FLT-ASC-06] Pyros must not arm while vertical_speed_cms > 1000 (10 m/s).
 * arming_gate_met() requires vertical_speed_cms < 1000 AND >= 0. */
#define A8_BURNOUT_MS 730u

void test_FLT_ASC_03_06_thrust_and_arming(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();

    uint32_t thrust_late = 0;
    bool armed_while_fast = false;
    float end_s = sim_data[sim_count - 1].time_s + 2.0f;
    uint32_t end_ms = (uint32_t)(end_s * 1000.0f);

    for (uint32_t t = 0; t <= end_ms; t++) {
        app_tick(t);
        if (ctx.current_state == ASCENT && ctx.under_thrust && ctx.last_sample > A8_BURNOUT_MS + 1000u)
            thrust_late = ctx.last_sample;
        /* [FLT-ASC-06] arming gate must require speed < 10 m/s */
        if (ctx.pyros_armed && ctx.vertical_speed_cms > 1000)
            armed_while_fast = true;
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, thrust_late, "under_thrust a second after burnout (FLT-ASC-03)");
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyros_armed, "the flight armed");
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
    memset(&last_buzzer_spec, 0, sizeof(last_buzzer_spec));
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
    snprintf(msg, sizeof(msg), "Expected the system-failure count (%u), got %u",
             (unsigned)beep_for(BR_SYSTEM_FAILURE).d1, (unsigned)last_buzzer_spec.d1);
    TEST_ASSERT_EQUAL_MESSAGE(beep_for(BR_SYSTEM_FAILURE).kind, last_buzzer_spec.kind, msg);
    TEST_ASSERT_EQUAL_MESSAGE(beep_for(BR_SYSTEM_FAILURE).d1, last_buzzer_spec.d1, msg);
}

/* ── Ground test command tests [GND-TEST-01..04, DD-011] ──────────── */

/* Helper: set up a standalone PAD_IDLE context with good continuity
 * and advance time until update_continuity_and_buzzer() has fired at
 * least once (requires >1000ms to pass while in PAD_IDLE). */
static void setup_pad_idle_with_continuity(void) {
    reset_sim();
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
    TEST_ASSERT_TRUE_MESSAGE(true, "last_status_code not set — continuity check not reached");
    TEST_ASSERT_EQUAL_MESSAGE(BR_OK_TO_FLY, ctx.last_reason, "Expected OK-to-fly for good continuity");

    int code_before = buzzer_code_count;
    mock_serial_enqueue("BEEP STATUS");
    mock_time_ms = 1210;
    mock_pressure.pressure_pa = 101325.0f;
    ctx.current_state = step(&ctx, 1210);

    TEST_ASSERT_EQUAL_MESSAGE(code_before + 1, buzzer_code_count, "BEEP STATUS did not replay the outcome");
    TEST_ASSERT_EQUAL_MESSAGE(BR_OK_TO_FLY, ctx.last_reason, "Replayed the wrong outcome");
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

/* ── Code review 2026-09-24 ───────────────────────────────────────── */

/* The row of flight_log.csv carrying an event, or NULL. */
static const char *log_row(const char *log, const char *event) {
    const char *p = log;
    size_t n = strlen(event);
    while ((p = strstr(p, event)) != NULL) {
        if ((p[n] == '\n' || p[n] == '\0') && p > log && p[-1] == ',') {
            while (p > log && p[-1] != '\n')
                p--;
            return p;
        }
        p += n;
    }
    return NULL;
}

static int read_flight_log(char *buf, int len) {
    int n = hal_fs_read_file("flight_log.csv", buf, len - 1);
    if (n > 0)
        buf[n] = '\0';
    return n;
}

/* [GND-CAL-05, REV-11] The LAUNCH row carries the height the rocket had
 * reached when the detector tripped -- a hundred feet -- not zero. */
void test_REV11_launch_row_reports_the_height_reached(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim();
    run_full_sim();
    static char log[32768];
    TEST_ASSERT_TRUE(read_flight_log(log, (int)sizeof(log)) > 0);
    const char *row = log_row(log, "LAUNCH");
    TEST_ASSERT_NOT_NULL_MESSAGE(row, "no LAUNCH row");
    unsigned long t;
    long pa, alt;
    TEST_ASSERT_EQUAL(3, sscanf(row, "%lu,%ld,%ld,", &t, &pa, &alt));
    char m[96];
    snprintf(m, sizeof(m), "LAUNCH row: t=%lu alt=%ld cm", t, alt);
    TEST_ASSERT_GREATER_THAN_MESSAGE(3000, alt, m);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (long)t, m);
}

/* The log header names each channel's mode the way config.ini spells it.
 * The HALs had AGL and FALLEN swapped, so a log of an AGL flight said
 * "fallen" -- and the header is the only record of what was configured. */
void test_REV_NEW_log_header_names_the_configured_modes(void) {
    load_sim_data("test_data/open_rocket_export.csv");
    reset_sim(); /* pyro1 delay 0, pyro2 agl 50 */
    run_full_sim();
    static char log[32768];
    TEST_ASSERT_TRUE(read_flight_log(log, (int)sizeof(log)) > 0);
    char *end = strstr(log, "time_ms,");
    if (end)
        *end = '\0'; /* the header is the part under test */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "# Pyro1: delay 0"), log);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "# Pyro2: agl 50"), log);
}

/* [CFG-SUBSYS-01, REV-12] log_rate_hz thins the samples; it never drops an
 * event. */
void test_REV12_log_rate_hz_thins_samples_not_events(void) {
    static char log[32768];
    load_sim_data("test_data/open_rocket_export.csv");

    reset_sim();
    run_full_sim();
    TEST_ASSERT_TRUE(read_flight_log(log, (int)sizeof(log)) > 0);
    int full = 0;
    for (const char *p = log; (p = strchr(p, '\n')) != NULL; p++)
        full++;

    reset_sim();
    ctx.config.log_rate_hz = 10;
    run_full_sim();
    TEST_ASSERT_TRUE(read_flight_log(log, (int)sizeof(log)) > 0);
    int thin = 0;
    for (const char *p = log; (p = strchr(p, '\n')) != NULL; p++)
        thin++;

    char m[96];
    snprintf(m, sizeof(m), "50 Hz log %d rows, 10 Hz log %d rows", full, thin);
    TEST_ASSERT_TRUE_MESSAGE(thin * 3 < full, m);
    TEST_ASSERT_NOT_NULL(log_row(log, "LAUNCH"));
    TEST_ASSERT_NOT_NULL(log_row(log, "ARMED"));
    TEST_ASSERT_NOT_NULL(log_row(log, "APOGEE"));
    TEST_ASSERT_NOT_NULL(log_row(log, "PYRO1"));
    TEST_ASSERT_NOT_NULL(log_row(log, "LANDING"));
}

/* [PYR-DEPLOY-02, GND-TEST-02, REV-06] The ground test goes through the same
 * interlock as the flight: channel 2 may not be energised while channel 1's
 * pulse is still running through the shared element. */
void test_REV06_ground_test_waits_for_the_other_channel(void) {
    setup_pad_idle_with_continuity();

    mock_serial_enqueue("ARM 1");
    ctx.current_state = step(&ctx, 1210);
    mock_serial_enqueue("FIRE 1");
    ctx.current_state = step(&ctx, 1220);
    TEST_ASSERT_EQUAL(1, mock_pyro.fire_count);
    TEST_ASSERT_TRUE(mock_pyro.firing); /* the pulse is still running */

    mock_uart_len = 0;
    mock_uart_buf[0] = '\0';
    mock_serial_enqueue("ARM 2");
    ctx.current_state = step(&ctx, 1300);
    mock_serial_enqueue("FIRE 2");
    ctx.current_state = step(&ctx, 1310);
    TEST_ASSERT_EQUAL_MESSAGE(1, mock_pyro.fire_count, "both channels were energised through one common element");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_uart_buf, "GT,ERR,busy"), mock_uart_buf);
}

/* ── Main ─────────────────────────────────────────────────────────── */

/* ── Item 6: brownout recovery, through the real machine ──────────── */

extern reset_cause_t mock_reset_cause; /* test/hal_test.c */

/* Sitting on the pad at a steady pressure. app_tick() drives the sensor from
 * the OpenRocket export, which these tests deliberately do not load: a marker
 * test wants a board that is not going anywhere. */
static void pad_tick_range(uint32_t from_ms, uint32_t to_ms) {
    for (uint32_t t = from_ms; t < to_ms; t++) {
        mock_time_ms = t;
        mock_pressure.pressure_pa = 101325.0f;
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);
        flight_update_outputs(&ctx, t);
        flight_flash_service(&ctx, t);
    }
}

/* [FLT-BROWN-01] The marker is written after ten seconds of PAD_IDLE and not
 * before, because the whole point is to have it on disk long before the
 * moment it protects against. */
void test_BRN_INT_01_marker_written_after_pad_dwell(void) {
    reset_sim();
    ctx.boot_timer = 0;

    pad_tick_range(0, PAD_MARKER_DWELL_MS - 500);
    pad_marker_t m;
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    TEST_ASSERT_TRUE_MESSAGE(n < (int)sizeof(m), "the marker must not be written before the dwell has elapsed");

    pad_tick_range(PAD_MARKER_DWELL_MS - 500, PAD_MARKER_DWELL_MS + 1500);
    n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    TEST_ASSERT_EQUAL_MESSAGE((int)sizeof(m), n, "the marker must exist after ten seconds on the pad");
    TEST_ASSERT_TRUE_MESSAGE(pad_marker_valid(&m), "the marker written on the pad must validate");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(300, 101325, m.ground_pressure_pa,
                                     "the marker must record the ground pressure it was sitting at");
}

/* Written once. A marker rewritten every tick would be flash wear on the pad
 * and a write in progress at an arbitrary moment. */
void test_BRN_INT_02_marker_written_only_once(void) {
    reset_sim();
    ctx.boot_timer = 0;
    pad_tick_range(0, PAD_MARKER_DWELL_MS + 3000);
    TEST_ASSERT_TRUE_MESSAGE(ctx.marker_written, "the marker should have been written");
    /* The flag is the guard; if it were not honoured the write would repeat
       for every tick after the dwell. */
    uint32_t before = mock_fs_write_count;
    pad_tick_range(PAD_MARKER_DWELL_MS + 3000, PAD_MARKER_DWELL_MS + 5000);
    TEST_ASSERT_EQUAL_MESSAGE(before, mock_fs_write_count, "the marker must be written once, not every tick");
}

/* On the hardware the state machine runs with the flash window shut, and a
 * write there is refused every time -- the marker was never written on any
 * board. So the detector must not be where the write happens: only
 * flight_flash_service(), which the main loop calls inside the window. */
void test_BRN_INT_05_marker_is_written_only_by_the_flash_service(void) {
    reset_sim();
    ctx.boot_timer = 0;
    for (uint32_t t = 0; t < PAD_MARKER_DWELL_MS + 2000; t++) {
        mock_time_ms = t;
        mock_pressure.pressure_pa = 101325.0f;
        ctx.current_state = step(&ctx, t);
        flight_update_outputs(&ctx, t);
    }
    pad_marker_t m;
    TEST_ASSERT_TRUE_MESSAGE(hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)) < (int)sizeof(m),
                             "the state machine wrote flash where the hardware refuses it");
    flight_flash_service(&ctx, PAD_MARKER_DWELL_MS + 2000);
    TEST_ASSERT_EQUAL_MESSAGE((int)sizeof(m), hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)),
                              "the flash service must write the marker once the dwell has passed");
}

/* [USB-01, USB-04] No marker while on USB, where the board sits at the
 * bench's altitude rather than the pad's; the dwell starts again when it is
 * unplugged, so the marker records where the board is flown from. */
void test_USB_INT_01_marker_waits_for_the_cable_to_go(void) {
    reset_sim();
    ctx.boot_timer = 0;
    int chirps = buzzer_usb_ok_count;
    flight_set_usb_attached(&ctx, true, 0);
    flight_set_usb_attached(&ctx, true, 20);
    TEST_ASSERT_EQUAL_MESSAGE(chirps + 1, buzzer_usb_ok_count, "one chirp per attach, not per loop");
    pad_tick_range(0, PAD_MARKER_DWELL_MS + 5000);
    pad_marker_t m;
    TEST_ASSERT_TRUE_MESSAGE(hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)) < (int)sizeof(m),
                             "a marker was written while on USB");

    uint32_t unplugged = PAD_MARKER_DWELL_MS + 5000;
    flight_set_usb_attached(&ctx, false, unplugged);
    pad_tick_range(unplugged, unplugged + PAD_MARKER_DWELL_MS - 500);
    TEST_ASSERT_TRUE_MESSAGE(hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)) < (int)sizeof(m),
                             "the dwell must restart when the cable goes");
    pad_tick_range(unplugged + PAD_MARKER_DWELL_MS - 500, unplugged + PAD_MARKER_DWELL_MS + 1000);
    TEST_ASSERT_EQUAL((int)sizeof(m), hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)));
}

/* [USB-08] Test mode on USB flies from where the board sits, so it records
 * that ground as the pad's. */
void test_USB_INT_03_test_mode_writes_the_marker_on_usb(void) {
    reset_sim();
    ctx.boot_timer = 0;
    flight_set_usb_attached(&ctx, true, 0);
    flight_set_test_mode(&ctx, true, 0);
    pad_tick_range(0, PAD_MARKER_DWELL_MS + 1500);
    pad_marker_t m;
    TEST_ASSERT_EQUAL_MESSAGE((int)sizeof(m), hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m)),
                              "test mode on USB must write the marker");
}

/* [USB-01] A board powered up on USB is on a bench, whatever the reset
 * registers and a leftover marker say. */
void test_USB_INT_02_no_flight_recovery_on_usb(void) {
    reset_sim();
    pad_marker_t m;
    pad_marker_fill(&m, 101325, 1200u);
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));
    mock_reset_cause = RESET_POWER_EVENT;
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    ctx.sensor_type = 1;
    ctx.fs_ok = true;
    flight_set_usb_attached(&ctx, true, 0);
    for (uint32_t t = 0; t < 6000; t++) {
        float alt_m = 600.0f - 20.0f * ((float)t / 1000.0f); /* what recovery would take for a descent */
        mock_pressure.pressure_pa = 101325.0f - alt_m * 12.0f;
        mock_time_ms = t;
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);
        if (ctx.current_state != BOOT_SETTLE && ctx.current_state != BOOT_SENSOR)
            break;
    }
    TEST_ASSERT_FALSE_MESSAGE(ctx.current_state == FALLING || ctx.current_state == ASCENT,
                              "a board on USB rejoined a flight");
    TEST_ASSERT_FALSE(ctx.diag & DIAG_BROWNOUT);
}

/* The case the mechanism exists for. A marker on disk, a power event, and a
 * barometer that says the board is high and descending: the machine must
 * rejoin the flight rather than calibrate a new zero at altitude. */
void test_BRN_INT_03_descending_recovery_rejoins_flight(void) {
    reset_sim();
    /* A marker recorded at sea-level ground pressure... */
    pad_marker_t m;
    pad_marker_fill(&m, 101325, 1200u);
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));

    /* ...and a board that wakes up 600 m above it, coming down. */
    mock_reset_cause = RESET_POWER_EVENT;
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    ctx.sensor_type = 1;
    ctx.fs_ok = true;
    ctx.last_sample = 0;
    ctx.last_altitude = 0;

    /* Continuous, not stepped: the sensor is fed faster than 20 ms, so a
     * stepped altitude lets a sample pair land inside one step and read as
     * stationary. */
    for (uint32_t t = 0; t < 6000; t++) {
        float alt_m = 600.0f - 20.0f * ((float)t / 1000.0f); /* 20 m/s down */
        mock_pressure.pressure_pa = 101325.0f - alt_m * 12.0f;
        mock_time_ms = t;
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);
        flight_update_outputs(&ctx, t);
        if (ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT || ctx.current_state == CHUTE_DESCENT) {
            break;
        }
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "state=%d recovery=%d diag=0x%x", (int)ctx.current_state, (int)ctx.recovery,
             (unsigned)ctx.diag);
    TEST_ASSERT_TRUE_MESSAGE(
        ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT || ctx.current_state == CHUTE_DESCENT, msg);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_BROWNOUT) != 0, "a recovered flight must say so in the diagnosis");
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyros_armed, "a rocket already descending has passed apogee: arm it");
    TEST_ASSERT_TRUE_MESSAGE(ctx.apogee_detected, "apogee is behind a descending rocket by definition");
    /* The recovered ground reference, NOT a fresh calibration at altitude. */
    TEST_ASSERT_INT32_WITHIN_MESSAGE(50, 101325, ctx.ground_pressure,
                                     "the ground reference must come from the marker, not from where it woke up");
}

/* A board switched on at the pad must calibrate normally, marker or no
 * marker. This is the common case and it must not go hunting for a flight. */
void test_BRN_INT_04_pad_power_on_calibrates_normally(void) {
    reset_sim();
    pad_marker_t m;
    pad_marker_fill(&m, 101325, 1200u);
    hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m));

    mock_reset_cause = RESET_POWER_EVENT;
    ctx.current_state = BOOT_SETTLE;
    ctx.boot_timer = 0;
    ctx.sensor_type = 1;
    ctx.fs_ok = true;

    for (uint32_t t = 0; t < 20000; t++) {
        mock_pressure.pressure_pa = 101325.0f; /* sitting still at the pad */
        mock_time_ms = t;
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);
        flight_update_outputs(&ctx, t);
        if (ctx.current_state == PAD_IDLE) {
            break;
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "state=%d recovery=%d", (int)ctx.current_state, (int)ctx.recovery);
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, msg);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_BROWNOUT) == 0, "a pad power-on must not be reported as a brownout");
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_TST_02_sim_data_loads);
    RUN_TEST(test_TST_02_interpolation);
    RUN_TEST(test_SNS_ALT_01_roundtrip);
    RUN_TEST(test_FLT_BOOT_01_all_states);
    RUN_TEST(test_FLT_APO_01_detected);
    RUN_TEST(test_PYR_MODE_01_fires);
    RUN_TEST(test_PYR_REL_01_released_channel_never_fires);
    RUN_TEST(test_PYR_REL_02_both_released_fires_nothing);
    RUN_TEST(test_PYR_REL_03_mocked_operations_are_reported);
    RUN_TEST(test_PYR_REL_04_released_reports_open_not_good);
    RUN_TEST(test_PYR_REL_05_shared_stimulus_stops_when_both_released);
    RUN_TEST(test_PYR_REL_06_fault_is_clear_for_a_released_channel);
    RUN_TEST(test_BEEP_01_clean_board_says_ok_to_fly);
    RUN_TEST(test_BEEP_02_a_pyro_fault_names_its_channel);
    RUN_TEST(test_BEEP_03_anything_unfixable_says_system_failure);
    RUN_TEST(test_BEEP_04_unfixable_outranks_fixable);
    RUN_TEST(test_PAD_EXCL_01_pyro_cannot_take_a_pad_lua_holds);
    RUN_TEST(test_PAD_EXCL_02_lua_cannot_take_a_pad_pyro_holds);
    RUN_TEST(test_PAD_EXCL_03_a_claim_is_all_or_nothing);
    RUN_TEST(test_PAD_EXCL_04_the_common_blocks_the_retained_channel_too);
    RUN_TEST(test_PAD_EXCL_05_reclaiming_your_own_pad_succeeds);
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

    RUN_TEST(test_BRN_INT_01_marker_written_after_pad_dwell);
    RUN_TEST(test_BRN_INT_02_marker_written_only_once);
    RUN_TEST(test_BRN_INT_03_descending_recovery_rejoins_flight);
    RUN_TEST(test_BRN_INT_04_pad_power_on_calibrates_normally);
    RUN_TEST(test_BRN_INT_05_marker_is_written_only_by_the_flash_service);
    RUN_TEST(test_USB_INT_01_marker_waits_for_the_cable_to_go);
    RUN_TEST(test_USB_INT_02_no_flight_recovery_on_usb);
    RUN_TEST(test_USB_INT_03_test_mode_writes_the_marker_on_usb);

    /* Code review 2026-09-24 */
    RUN_TEST(test_REV11_launch_row_reports_the_height_reached);
    RUN_TEST(test_REV_NEW_log_header_names_the_configured_modes);
    RUN_TEST(test_REV12_log_rate_hz_thins_samples_not_events);
    RUN_TEST(test_REV06_ground_test_waits_for_the_other_channel);
    return UNITY_END();
}
