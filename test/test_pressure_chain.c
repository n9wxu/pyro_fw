/*
 * The pressure chain under a real sensor's noise: from the reading to the
 * detectors, booted the way the hardware boots.
 *
 * Every other suite feeds a perfectly smooth pressure and primes the pressure
 * layer before booting. Here the board boots from BOOT_SETTLE with nothing
 * primed, the sensor has the MS5607's noise (1.2 Pa RMS at OSR 4096,
 * truncated to whole pascals as hal_common.c does), and a flight's truth is
 * integrated beside the firmware so each detection can be timed against what
 * really happened. Each scenario prints the figure its row in
 * docs/outstanding_tasks.md's baseline table names.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/flight_states.h"
#include "../src/brownout.h"
#include "../src/buzzer.h"
#include "../src/hal.h"
#include "../src/telemetry_formatter.h"
#include "pressure_processing.h"

/* ── Buzzer stand-in: the announcements are not under test here ──── */

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

void setUp(void) {}
void tearDown(void) {}

/* ── The truth ────────────────────────────────────────────────────── */

#define G 9.80665f
#define SENSOR_RMS_PA 1.2f /* MS5607 at OSR 4096 */
#define PAD_PA 101325.0f
/* Pascals per metre at the pad: ρg with the ISA's 1.225 kg/m³. */
#define PA_PER_M 12.01f

static float isa_pa(float h_m) {
    return PAD_PA * powf(1.0f - 2.25577e-5f * h_m, 5.25588f);
}

typedef struct {
    float g_net;  /* boost, net of gravity, in g */
    float burn_s; /* 0: the rocket never leaves the pad */
    float v_term; /* descent under canopy, m/s */
    float land_m; /* the landing site's height above the pad */
} flight_t;

typedef struct {
    float h, v;
    bool apogee, down;
} truth_t;

/* Drag-free boost and coast, then a canopy's quadratic drag toward v_term. */
static void truth_step(truth_t *tr, const flight_t *f, float t_flight_s) {
    const float dt = 0.001f;
    if (tr->down || t_flight_s < 0.0f || f->burn_s <= 0.0f)
        return;
    float a = -G;
    if (t_flight_s < f->burn_s)
        a += f->g_net * G + G;
    if (tr->apogee && tr->v < 0.0f)
        a += G * (tr->v / f->v_term) * (tr->v / f->v_term);
    tr->v += a * dt;
    tr->h += tr->v * dt;
    if (tr->h <= f->land_m && t_flight_s > f->burn_s) {
        tr->h = f->land_m;
        tr->v = 0.0f;
        tr->down = true;
    }
}

/* ── The board ────────────────────────────────────────────────────── */

static flight_context_t ctx;
extern reset_cause_t mock_reset_cause; /* test/hal_test.c */

/* Boots the way the hardware does: nothing primed, from BOOT_SETTLE. */
static void boot_like_hardware(uint32_t seed) {
    mock_reset_all();
    pp_init();
    memset(&ctx, 0, sizeof(ctx));
    config_set_defaults(&ctx.config);
    telemetry_init(&ctx.config);
    ctx.current_state = BOOT_SETTLE;
    ctx.sensor_type = 1;
    ctx.fs_ok = true;
    mock_pressure.pressure_pa = PAD_PA;
    mock_noise_rms_pa = SENSOR_RMS_PA;
    mock_noise_seed = seed;
    mock_stall_seed = seed * 7919u + 1u;
}

/* One pass of the main loop, in its order (main_hardware.c). */
static void tick(uint32_t t) {
    mock_time_ms = t;
    mock_pyro.firing = false;
    hal_tasks_tick(t);
    if (mock_core0_stalled(t))
        return;
    ctx.current_state = dispatch_state(&ctx, t);
    flight_update_outputs(&ctx, t);
    flight_flash_service(&ctx, t);
}

/* Runs the loop until PAD_IDLE; returns the time it got there. */
static uint32_t run_to_pad(uint32_t *t) {
    for (; *t < 20000; (*t)++) {
        tick(*t);
        if (ctx.current_state == PAD_IDLE)
            return *t;
    }
    TEST_FAIL_MESSAGE("never reached PAD_IDLE");
    return 0;
}

typedef struct {
    uint32_t ignition_ms, launch_ms, apogee_true_ms, apogee_ms, touchdown_ms, landed_ms;
    int32_t ground_frozen_pa;
    double speed_err_max, speed_err_sq;
    int speed_err_n;
} result_t;

/* A flight from power-on to LANDED, or to until_ms. */
static result_t fly(const flight_t *f, uint32_t seed, uint32_t pad_s, uint32_t until_ms, bool stalls) {
    result_t r;
    memset(&r, 0, sizeof(r));
    boot_like_hardware(seed);
    mock_stall_model = stalls;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    r.ignition_ms = pad + pad_s * 1000u;
    truth_t tr = {0};
    uint32_t last_sample = ctx.last_sample;
    for (; t < until_ms; t++) {
        float tf = ((float)t - (float)r.ignition_ms) / 1000.0f;
        float v_before = tr.v;
        truth_step(&tr, f, tf);
        if (!tr.apogee && tf > f->burn_s && v_before > 0.0f && tr.v <= 0.0f) {
            tr.apogee = true;
            r.apogee_true_ms = t;
        }
        if (tr.down && r.touchdown_ms == 0)
            r.touchdown_ms = t;
        mock_pressure.pressure_pa = isa_pa(tr.h);
        tick(t);
        if (ctx.current_state == ASCENT && r.launch_ms == 0) {
            r.launch_ms = t;
            r.ground_frozen_pa = ctx.ground_pressure;
        }
        if (ctx.current_state == ASCENT && ctx.last_sample != last_sample && tr.v >= 90.0f && tr.v <= 110.0f) {
            double e = fabs(ctx.vertical_speed_cms / 100.0 - tr.v);
            r.speed_err_sq += e * e;
            r.speed_err_n++;
            if (e > r.speed_err_max)
                r.speed_err_max = e;
        }
        last_sample = ctx.last_sample;
        if (ctx.apogee_detected && r.apogee_ms == 0)
            r.apogee_ms = t;
        if (ctx.current_state == LANDED) {
            r.landed_ms = t;
            break;
        }
    }
    return r;
}

/* ── T0: the support itself ───────────────────────────────────────── */

void test_T0_noise_model(void) {
    mock_reset_all();
    pp_init();
    mock_noise_rms_pa = SENSOR_RMS_PA;
    mock_noise_seed = 42;
    mock_pressure.pressure_pa = PAD_PA;
    int32_t first[16];
    double sq = 0.0;
    int n = 0;
    for (uint32_t t = 1; n < 10000; t++) {
        hal_tasks_tick(t * 20u);
        int32_t raw = pp_last_raw_pa();
        if (n < 16)
            first[n] = raw;
        /* Truncated, as the HAL does: the error includes the fraction lost. */
        double e = (double)raw - PAD_PA;
        sq += e * e;
        n++;
    }
    double rms = sqrt(sq / n);
    /* Truncation toward zero adds a mean of -0.5 Pa and 0.29 Pa of spread. */
    double expect = sqrt(SENSOR_RMS_PA * SENSOR_RMS_PA + 1.0 / 12.0 + 0.25);
    printf("  noise model: RMS %.3f Pa after truncation (expected %.3f)\n", rms, expect);
    TEST_ASSERT_FLOAT_WITHIN(0.02 * expect, expect, rms);

    mock_reset_all();
    pp_init();
    mock_noise_rms_pa = SENSOR_RMS_PA;
    mock_noise_seed = 42;
    mock_pressure.pressure_pa = PAD_PA;
    for (int i = 0; i < 16; i++) {
        hal_tasks_tick((uint32_t)(i + 1) * 20u);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(first[i], pp_last_raw_pa(), "the same seed must give the same noise");
    }
}

void test_T0_stall_model(void) {
    mock_reset_all();
    pp_init();
    mock_stall_model = true;
    mock_stall_seed = 3;
    mock_pressure.pressure_pa = PAD_PA;
    for (uint32_t t = 1; t <= 300000; t++)
        hal_tasks_tick(t);
    double per_s = mock_stall_count / 300.0;
    printf("  stall model: %u stalls in 300 s (%.2f/s), %u-%u ms; stamp lag %u-%u ms\n", (unsigned)mock_stall_count,
           per_s, (unsigned)mock_stall_min_ms, (unsigned)mock_stall_max_ms, (unsigned)mock_stamp_lag_min_ms,
           (unsigned)mock_stamp_lag_max_ms);
    TEST_ASSERT_TRUE_MESSAGE(per_s > 1.15 && per_s < 1.45, "about 1.3 stalls a second");
    TEST_ASSERT_TRUE(mock_stall_min_ms >= 40 && mock_stall_max_ms <= 73);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(16, mock_stamp_lag_min_ms, "normally stamped 16 ms after the conversion");
    TEST_ASSERT_TRUE_MESSAGE(mock_stamp_lag_max_ms >= 16 + 40,
                             "a conversion commanded before a stall is read, and stamped, after it");
}

void test_T0_off_by_default(void) {
    mock_reset_all();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mock_noise_rms_pa);
    TEST_ASSERT_EQUAL_INT(0, mock_glitch_samples);
    TEST_ASSERT_FALSE(mock_stall_model);
    pp_init();
    mock_pressure.pressure_pa = 100000.7f;
    hal_tasks_tick(20);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(100000, pp_last_raw_pa(), "with the models off, the reading is the pressure");
}

void test_T0_unprimed_boot(void) {
    boot_like_hardware(1);
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    printf("  booted as the hardware boots: PAD_IDLE at %u ms, ground %ld Pa\n", (unsigned)pad,
           (long)pp_ground_pressure());
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_INT_WITHIN(3, (int32_t)PAD_PA, pp_ground_pressure());
}

/* ── T0: today's numbers ──────────────────────────────────────────── */

/* Speed noise on the pad: the speed the launch detector reads. */
void test_T0_baseline_pad_speed_noise(void) {
    boot_like_hardware(11);
    uint32_t t = 0;
    run_to_pad(&t);
    uint32_t end = t + 60000u, last = ctx.last_sample;
    double sq = 0.0;
    int n = 0;
    for (; t < end; t++) {
        tick(t);
        if (ctx.last_sample != last) {
            double v = ctx.pad_speed_cms / 100.0;
            sq += v * v;
            n++;
            last = ctx.last_sample;
        }
    }
    printf("  BASELINE pad speed noise: %.2f m/s RMS over %d samples\n", sqrt(sq / n), n);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
}

/* Touchdown to LANDED under a 5 m/s canopy, by the stillness test alone, on
 * the pad's own level and 5 m above it. The launch freezes a ground reference a
 * little low (T7), so on the pad's level the altitude sits at the zero clamp,
 * where the noise cannot reach it. */
static void touchdown_to_landed(float land_m) {
    const flight_t f = {5.0f, 1.0f, 5.0f, land_m};
    double sum = 0.0, worst = 0.0;
    int n = 0, never = 0;
    /* The timeout is a config value, so this flight is flown by hand with it
     * off: fly() boots with the defaults. */
    for (uint32_t seed = 1; seed <= 20; seed++) {
        boot_like_hardware(seed);
        ctx.config.landing_timeout = 0;
        uint32_t t = 0;
        uint32_t pad = run_to_pad(&t);
        uint32_t ign = pad + 3000u, touchdown = 0, landed = 0;
        truth_t tr = {0};
        for (; t < 400000u; t++) {
            float tf = ((float)t - (float)ign) / 1000.0f;
            float vb = tr.v;
            truth_step(&tr, &f, tf);
            if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f)
                tr.apogee = true;
            if (tr.down && touchdown == 0)
                touchdown = t;
            mock_pressure.pressure_pa = isa_pa(tr.h);
            tick(t);
            if (ctx.current_state == LANDED) {
                landed = t;
                break;
            }
        }
        if (landed == 0 || touchdown == 0 || landed < touchdown) {
            never++;
            continue;
        }
        double s = (landed - touchdown) / 1000.0;
        sum += s;
        if (s > worst)
            worst = s;
        n++;
    }
    printf("  BASELINE touchdown to LANDED, %2.0f m above the pad: %.1f s mean, %.1f s worst, %d flights (%d not "
           "landed within 400 s, or before touchdown)\n",
           land_m, n ? sum / n : 0.0, worst, n, never);
}

void test_T0_baseline_touchdown_to_landed(void) {
    touchdown_to_landed(0.0f);
    touchdown_to_landed(5.0f);
}

/* Apogee declared after the true apogee. */
void test_T0_baseline_apogee_delay(void) {
    const flight_t f = {5.0f, 2.0f, 20.0f, 0.0f};
    double sum = 0.0, lo = 1e9, hi = -1e9;
    int n = 0;
    for (uint32_t seed = 1; seed <= 20; seed++) {
        result_t r = fly(&f, seed, 3, 60000, false);
        if (r.apogee_ms == 0 || r.apogee_true_ms == 0)
            continue;
        double d = ((double)r.apogee_ms - (double)r.apogee_true_ms) / 1000.0;
        sum += d;
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
        n++;
    }
    printf("  BASELINE apogee after the true apogee: %+.2f s mean (%+.2f to %+.2f) over %d flights\n",
           n ? sum / n : 0.0, lo, hi, n);
    TEST_ASSERT_TRUE(n > 0);
}

/* The ground pressure the launch froze, against the pad's true pressure. */
void test_T0_baseline_ground_at_launch(void) {
    const float gs[] = {2.0f, 5.0f, 15.0f, 30.0f};
    for (unsigned i = 0; i < sizeof(gs) / sizeof(gs[0]); i++) {
        flight_t f = {gs[i], 3.0f, 20.0f, 0.0f};
        double sum = 0.0;
        int n = 0;
        for (uint32_t seed = 1; seed <= 10; seed++) {
            result_t r = fly(&f, seed, 10, 20000, false);
            if (r.launch_ms == 0)
                continue;
            sum += (PAD_PA - (double)r.ground_frozen_pa) / PA_PER_M;
            n++;
        }
        printf("  BASELINE %2.0f g launch: every AGL reads %.2f m low (ground frozen from the climb), %d flights\n",
               gs[i], n ? sum / n : 0.0, n);
    }
}

/* A step on the pad bigger than the 50 Pa gate. */
void test_T0_baseline_ground_step(void) {
    boot_like_hardware(5);
    uint32_t t = 0;
    run_to_pad(&t);
    uint32_t end = t + 10000u;
    for (; t < end; t++)
        tick(t);
    mock_pressure.pressure_pa = PAD_PA - 60.0f; /* carried 5 m up */
    end = t + 60000u;
    for (; t < end; t++)
        tick(t);
    printf("  BASELINE ground reference 60 s after a 60 Pa step: %ld Pa off\n",
           (long)(pp_ground_pressure() - (int32_t)(PAD_PA - 60.0f)));
}

/* One glitch on the pad: which sizes declare a launch. */
void test_T0_baseline_pad_glitch(void) {
    const int32_t sizes[] = {-60000, -40000, -20000, -15000, -13000, -12000, -11000, -8000,
                             -5000,  -2000,  2000,   5000,   10000,  15000,  18000};
    char launched[256] = "", held[256] = "";
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        boot_like_hardware(9);
        uint32_t t = 0;
        run_to_pad(&t);
        uint32_t end = t + 5000u;
        for (; t < end; t++)
            tick(t);
        mock_glitch_pa = sizes[i];
        mock_glitch_samples = 1;
        end = t + 3000u;
        for (; t < end; t++)
            tick(t);
        char *dst = ctx.current_state == PAD_IDLE ? held : launched;
        char item[16];
        snprintf(item, sizeof(item), " %+ld", (long)(sizes[i] / 1000));
        strncat(dst, item, 255 - strlen(dst));
    }
    printf("  BASELINE one glitch on the pad (kPa): launches at%s; holds at%s\n", launched, held);
}

/* Brownout recovery, booted as the hardware boots. */
void test_T0_baseline_recovery_unprimed(void) {
    boot_like_hardware(3);
    pad_marker_t m;
    pad_marker_fill(&m, (int32_t)PAD_PA);
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));
    mock_reset_cause = RESET_POWER_EVENT;
    for (uint32_t t = 0; t < 8000; t++) {
        mock_pressure.pressure_pa = isa_pa(600.0f - 20.0f * (float)t / 1000.0f);
        tick(t);
        if (ctx.current_state != BOOT_SETTLE && ctx.current_state != BOOT_SENSOR)
            break;
    }
    printf("  BASELINE recovery 600 m up and descending, booted unprimed: %s (state %d)\n",
           brownout_recovery_name((recovery_t)ctx.recovery), (int)ctx.current_state);
}

/* Speed error at 100 m/s, with and without core0's flash stalls. */
void test_T0_baseline_stall_speed_error(void) {
    const flight_t f = {15.0f, 1.2f, 20.0f, 0.0f}; /* about 176 m/s at burnout; passes 100 m/s in coast */
    for (int stalls = 0; stalls <= 1; stalls++) {
        double worst = 0.0, sq = 0.0;
        int n = 0;
        for (uint32_t seed = 1; seed <= 10; seed++) {
            result_t r = fly(&f, seed, 3, 30000, stalls != 0);
            sq += r.speed_err_sq;
            n += r.speed_err_n;
            if (r.speed_err_max > worst)
                worst = r.speed_err_max;
        }
        printf("  BASELINE speed error at 90-110 m/s %s stalls: %.1f m/s worst, %.1f m/s RMS\n",
               stalls ? "with" : "without", worst, n ? sqrt(sq / n) : 0.0);
    }
}

/* A main at 5 m/s: the landing timeout (N7). */
void test_T0_baseline_landing_under_main(void) {
    const flight_t f = {5.0f, 3.0f, 5.0f, 0.0f}; /* apogee about 1 km: over 3 minutes on the main */
    result_t r = fly(&f, 2, 3, 400000, false);
    if (r.landed_ms && r.touchdown_ms)
        printf("  BASELINE main at 5 m/s: LANDED %.1f s after touchdown\n",
               ((double)r.landed_ms - (double)r.touchdown_ms) / 1000.0);
    else if (r.landed_ms)
        printf("  BASELINE main at 5 m/s: LANDED in the air, %.1f s after apogee\n",
               ((double)r.landed_ms - (double)r.apogee_true_ms) / 1000.0);
    else
        printf("  BASELINE main at 5 m/s: never LANDED\n");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_T0_noise_model);
    RUN_TEST(test_T0_stall_model);
    RUN_TEST(test_T0_off_by_default);
    RUN_TEST(test_T0_unprimed_boot);
    RUN_TEST(test_T0_baseline_pad_speed_noise);
    RUN_TEST(test_T0_baseline_touchdown_to_landed);
    RUN_TEST(test_T0_baseline_apogee_delay);
    RUN_TEST(test_T0_baseline_ground_at_launch);
    RUN_TEST(test_T0_baseline_ground_step);
    RUN_TEST(test_T0_baseline_pad_glitch);
    RUN_TEST(test_T0_baseline_recovery_unprimed);
    RUN_TEST(test_T0_baseline_stall_speed_error);
    RUN_TEST(test_T0_baseline_landing_under_main);
    return UNITY_END();
}
