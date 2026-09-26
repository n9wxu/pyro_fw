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

/* What a test sets before power-on, applied after flight_init() as the
 * hardware's main loop would find it. */
static struct {
    bool powered;
    bool usb;
    int landing_timeout; /* -1: the default */
} power;

/* Boots the way the hardware does: nothing primed, from BOOT_SETTLE. The
 * first tick runs flight_init(), which reads the reset cause and the sensor,
 * so anything a test sets up before it -- a marker, a reset cause -- is what
 * the board finds at power-on. */
static void boot_like_hardware(uint32_t seed) {
    mock_reset_all();
    memset(&ctx, 0, sizeof(ctx));
    memset(&power, 0, sizeof(power));
    power.landing_timeout = -1;
    mock_pressure.pressure_pa = PAD_PA;
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

/* One pass of the main loop, in its order (main_hardware.c). */
static void tick(uint32_t t) {
    if (!power.powered)
        power_on();
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

/* One glitch during the next fly(), at a flight time; cleared by each fly(). */
static struct {
    float at_s;
    int32_t pa;
    int samples;
} glitch;

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
    bool glitched = false;
    for (; t < until_ms; t++) {
        float tf = ((float)t - (float)r.ignition_ms) / 1000.0f;
        if (glitch.samples > 0 && !glitched && tf >= glitch.at_s) {
            mock_glitch_pa = glitch.pa;
            mock_glitch_samples = glitch.samples;
            glitched = true;
        }
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
    memset(&glitch, 0, sizeof(glitch));
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
        power.landing_timeout = 0;
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

/* ── T2: a single-sample glitch never reaches the filter ──────────── */

/* The sizes a flipped high bit in the raw reading could produce, both ways. */
static const int32_t GLITCHES[] = {-60000, -40000, -20000, -15000, -12000, -11000, -8000,
                                   -5000,  -2000,  2000,   5000,   10000,  15000,  18000};
#define N_GLITCHES (sizeof(GLITCHES) / sizeof(GLITCHES[0]))

void test_T2_pad_glitch_sweep(void) {
    char launched[256] = "";
    for (unsigned i = 0; i < N_GLITCHES; i++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            boot_like_hardware(seed);
            uint32_t t = 0;
            run_to_pad(&t);
            uint32_t end = t + 5000u + 7u * seed;
            for (; t < end; t++)
                tick(t);
            mock_glitch_pa = GLITCHES[i];
            mock_glitch_samples = 1;
            end = t + 3000u;
            for (; t < end; t++)
                tick(t);
            if (ctx.current_state != PAD_IDLE) {
                char item[24];
                snprintf(item, sizeof(item), " %+ld(seed %u)", (long)(GLITCHES[i] / 1000), (unsigned)seed);
                strncat(launched, item, sizeof(launched) - 1 - strlen(launched));
            }
        }
    }
    if (launched[0])
        printf("  one glitch on the pad launched at (kPa):%s\n", launched);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", launched, "one glitch must never declare a launch");
}

/* A glitch high in pressure reads as a sudden drop: in coast, once armed, an
 * apogee. The glitch is swept across the last second before the true apogee,
 * on a flight that latches the Mach gate and one that stays under it: the
 * gate holds apogee off until 1 s after arming, which on the first flight is
 * after the true apogee anyway. */
void test_T2_coast_glitch(void) {
    const flight_t flights[] = {
        {5.0f, 2.0f, 20.0f, 0.0f},  /* 98 m/s at burnout, apogee 588 m at 12.0 s */
        {5.0f, 0.55f, 20.0f, 0.0f}, /* 27 m/s, under the gate's 30.5 m/s; apogee 45 m at 3.3 s */
    };
    const int32_t sizes[] = {2000, 5000, 10000, 20000};
    char early[512] = "";
    for (unsigned fi = 0; fi < 2; fi++) {
        const flight_t *f = &flights[fi];
        float t_ap = f->burn_s + f->g_net * f->burn_s;
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            for (int k = 1; k <= 20; k++) {
                glitch.at_s = t_ap - 0.05f * (float)k;
                glitch.pa = sizes[i];
                glitch.samples = 1;
                result_t r = fly(f, 4, 3, 40000, false);
                if (r.apogee_ms != 0 && r.apogee_ms < r.apogee_true_ms) {
                    char item[64];
                    snprintf(item, sizeof(item), " [flight %u, +%ld kPa, %.2f s before: %ld ms early]", fi,
                             (long)(sizes[i] / 1000), 0.05 * k, (long)r.apogee_true_ms - (long)r.apogee_ms);
                    strncat(early, item, sizeof(early) - 1 - strlen(early));
                }
            }
        }
    }
    if (early[0])
        printf("  early apogees:%s\n", early);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", early, "one glitch in coast must never declare apogee early");
}

void test_T2_calibration_glitch(void) {
    boot_like_hardware(6);
    uint32_t t = 0;
    bool glitched = false;
    for (; t < 20000 && ctx.current_state != PAD_IDLE; t++) {
        if (ctx.current_state == BOOT_CALIBRATE && !glitched) {
            mock_glitch_pa = -20000;
            mock_glitch_samples = 1;
            glitched = true;
        }
        tick(t);
    }
    TEST_ASSERT_TRUE(glitched);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    uint32_t end = t + 10000u;
    for (; t < end; t++)
        tick(t);
    int32_t err = pp_ground_pressure() - (int32_t)PAD_PA;
    char msg[64];
    snprintf(msg, sizeof(msg), "ground %ld Pa from the pad's pressure", (long)err);
    TEST_ASSERT_TRUE_MESSAGE(err >= -1 && err <= 1, msg);
}

/* A straight ramp is monotonic, so its median is always the middle sample:
 * the stage costs one sample of latency and changes nothing else. Delivered
 * with the middle sample's own time, the altitudes come out exactly as the
 * ramp without the spike gives them. */
typedef struct {
    int32_t alt;
    uint32_t ts;
} out_t;

static int ramp_through_pp(bool spike, out_t *out, int cap, uint32_t *newest_ts_after_k20) {
    pp_init();
    pp_test_prime(101325);
    int n = 0;
    for (int k = 1; k <= 50; k++) {
        int32_t p = 101325 - 10 * k;
        if (spike && k == 25)
            p -= 5000;
        pp_feed(p, (uint32_t)k * 20u);
        altitude_sample_t a;
        while (pp_read(&a) && n < cap) {
            out[n].alt = a.altitude_cm;
            out[n].ts = a.timestamp_ms;
            n++;
        }
        if (k == 20)
            *newest_ts_after_k20 = n ? out[n - 1].ts : 0;
    }
    return n;
}

void test_T2_median_timing(void) {
    static out_t clean[64], spiked[64];
    uint32_t newest_clean = 0, newest_spiked = 0;
    int nc = ramp_through_pp(false, clean, 64, &newest_clean);
    int ns = ramp_through_pp(true, spiked, 64, &newest_spiked);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(19u * 20u, newest_clean,
                                     "after the 20th sample, the newest out is the 19th: one sample late");
    TEST_ASSERT_EQUAL_INT(nc, ns);
    for (int i = 0; i < nc; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(clean[i].ts, spiked[i].ts, "the spike moved a timestamp");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(clean[i].alt, spiked[i].alt, "the spike reached the filter");
    }
}

/* ── T1: brownout recovery from real samples (N23, N25) ───────────── */

static void write_marker(int32_t ground_pa) {
    pad_marker_t m;
    pad_marker_fill(&m, ground_pa);
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));
}

static bool marker_valid(void) {
    pad_marker_t m;
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    return n == (int)sizeof(m) && pad_marker_valid(&m);
}

static bool booting(void) {
    return ctx.current_state == BOOT_SETTLE || ctx.current_state == BOOT_SENSOR;
}

static bool descending_state(void) {
    return ctx.current_state == FALLING || ctx.current_state == DROGUE_DESCENT || ctx.current_state == CHUTE_DESCENT;
}

/* A board that powers up in the air, against a marker from the pad. The truth
 * starts at h0 moving at v0, coasts ballistic, and falls under a 20 m/s canopy
 * once past apogee. Returns the truth's apogee time (0 if it was already
 * descending) and records when each channel fired, and at what height. */
typedef struct {
    uint32_t apogee_ms, fire_ms[3];
    float fire_h[3];
    flight_state_t rejoined;
} air_boot_t;

static air_boot_t boot_in_the_air(float h0, float v0, uint32_t seed, uint32_t until_ms) {
    air_boot_t a;
    memset(&a, 0, sizeof(a));
    boot_like_hardware(seed);
    write_marker((int32_t)PAD_PA);
    mock_reset_cause = RESET_POWER_EVENT;
    float h = h0, v = v0;
    int fires = 0;
    for (uint32_t t = 0; t < until_ms; t++) {
        float vb = v;
        float acc = -G;
        if (v < 0.0f)
            acc += G * (v / 20.0f) * (v / 20.0f);
        v += acc * 0.001f;
        h += v * 0.001f;
        if (vb > 0.0f && v <= 0.0f)
            a.apogee_ms = t;
        if (h < 0.0f)
            h = 0.0f;
        mock_pressure.pressure_pa = isa_pa(h);
        bool was_booting = booting();
        tick(t);
        if (was_booting && !booting())
            a.rejoined = ctx.current_state;
        while (fires < mock_pyro.fire_count) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch <= 2 && a.fire_ms[ch] == 0) {
                a.fire_ms[ch] = t;
                a.fire_h[ch] = h;
            }
            fires++;
        }
        if (h <= 0.0f && t > 1000)
            break;
    }
    return a;
}

void test_T1_rejoins_descent(void) {
    air_boot_t a = boot_in_the_air(600.0f, -20.0f, 1, 60000);
    char msg[128];
    snprintf(msg, sizeof(msg), "rejoined in state %d, recovery '%s'", (int)a.rejoined, flight_recovery_text(&ctx));
    TEST_ASSERT_TRUE_MESSAGE(a.rejoined == FALLING || a.rejoined == DROGUE_DESCENT || a.rejoined == CHUTE_DESCENT, msg);
    /* And the resumed flight deploys: the default main is 300 m AGL. */
    snprintf(msg, sizeof(msg), "main fired at %.0f m (0: never)", (double)a.fire_h[2]);
    TEST_ASSERT_TRUE_MESSAGE(a.fire_ms[2] != 0 && fabsf(a.fire_h[2] - 300.0f) < 20.0f, msg);
}

void test_T1_rejoins_ascent(void) {
    air_boot_t a = boot_in_the_air(300.0f, 50.0f, 2, 60000);
    char msg[128];
    snprintf(msg, sizeof(msg), "rejoined in state %d, recovery '%s'", (int)a.rejoined, flight_recovery_text(&ctx));
    TEST_ASSERT_EQUAL_MESSAGE(ASCENT, a.rejoined, msg);
    /* The drogue (apogee, no delay) fires after the true apogee, not long after. */
    snprintf(msg, sizeof(msg), "drogue %ld ms after the true apogee (fire at %u)",
             (long)a.fire_ms[1] - (long)a.apogee_ms, (unsigned)a.fire_ms[1]);
    TEST_ASSERT_TRUE_MESSAGE(a.fire_ms[1] != 0 && a.fire_ms[1] >= a.apogee_ms && a.fire_ms[1] - a.apogee_ms < 1500,
                             msg);
}

/* guard: FLT-BROWN-03. Passes on the old code only because recovery never ran. */
void test_T1_still_board_stays_cold(void) {
    int resumed = 0;
    for (uint32_t seed = 1; seed <= 1000; seed++) {
        boot_like_hardware(seed);
        write_marker((int32_t)PAD_PA);
        mock_reset_cause = RESET_POWER_EVENT;
        mock_pressure.pressure_pa = isa_pa(40.0f);
        for (uint32_t t = 0; t < 8000 && booting(); t++)
            tick(t);
        if (ctx.recovery == RECOVER_ASCENT || ctx.recovery == RECOVER_DESCENT)
            resumed++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, resumed, "a still board 40 m up must never resume a flight");
}

/* guard: N25. One or two glitches anywhere in the history, on the pad, never
 * make a flight. Passes on the old code only because recovery never ran. */
void test_T1_glitch_on_the_pad(void) {
    const int32_t sizes[] = {-60000, -20000, -12000, -5000, 5000, 12000, 18000};
    char resumed[512] = "";
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int pair = 1; pair <= 2; pair++) {
            for (uint32_t k = 0; k <= 40; k++) {
                boot_like_hardware(100 + k);
                write_marker((int32_t)PAD_PA);
                mock_reset_cause = RESET_POWER_EVENT;
                uint32_t at = 2520u - 20u * k;
                for (uint32_t t = 0; t < 8000 && booting(); t++) {
                    if (t == at) {
                        mock_glitch_pa = sizes[i];
                        mock_glitch_samples = pair;
                    }
                    tick(t);
                }
                if (ctx.recovery == RECOVER_ASCENT || ctx.recovery == RECOVER_DESCENT) {
                    char item[48];
                    snprintf(item, sizeof(item), " [%+ld kPa x%d at %u ms]", (long)(sizes[i] / 1000), pair,
                             (unsigned)at);
                    strncat(resumed, item, sizeof(resumed) - 1 - strlen(resumed));
                }
            }
        }
    }
    if (resumed[0])
        printf("  resumed a flight on the pad:%s\n", resumed);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", resumed, "a glitch on the pad must never resume a flight");
}

static const char *cold_reason(bool marker, bool usb, bool samples, reset_cause_t cause, float h) {
    boot_like_hardware(7);
    if (marker)
        write_marker((int32_t)PAD_PA);
    mock_reset_cause = cause;
    power.usb = usb;
    mock_pressure.pressure_pa = isa_pa(h);
    for (uint32_t t = 0; t < 10000 && booting(); t++) {
        tick(t);
        if (!samples)
            mock_pressure.sensor_type = 0; /* answered at init, then nothing */
    }
    return flight_recovery_text(&ctx);
}

void test_T1_cold_reasons(void) {
    TEST_ASSERT_EQUAL_STRING("cold: no marker", cold_reason(false, false, true, RESET_POWER_EVENT, 0.0f));
    TEST_ASSERT_EQUAL_STRING("cold: on USB", cold_reason(true, true, true, RESET_POWER_EVENT, 0.0f));
    TEST_ASSERT_EQUAL_STRING("cold: at ground level", cold_reason(true, false, true, RESET_POWER_EVENT, 0.0f));
    TEST_ASSERT_EQUAL_STRING("cold: no sample in time", cold_reason(true, false, false, RESET_POWER_EVENT, 0.0f));
    TEST_ASSERT_EQUAL_STRING("cold: not a power event", cold_reason(true, false, true, RESET_SOFTWARE, 0.0f));
}

/* N25: a marker outlives nothing but its own flight. */
void test_T1_marker_invalid_after_landing(void) {
    const flight_t f = {5.0f, 1.0f, 5.0f, 0.0f};
    boot_like_hardware(8);
    mock_reset_cause = RESET_POWER_EVENT;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    uint32_t ign = pad + 12000u;
    truth_t tr = {0};
    uint32_t landed = 0;
    bool had_marker = false;
    for (; t < 300000u; t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        float vb = tr.v;
        truth_step(&tr, &f, tf);
        if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f)
            tr.apogee = true;
        mock_pressure.pressure_pa = isa_pa(tr.h);
        tick(t);
        if (t == ign)
            had_marker = marker_valid();
        if (ctx.current_state == LANDED && landed == 0)
            landed = t;
        if (landed && t > landed + 200u)
            break;
    }
    TEST_ASSERT_TRUE_MESSAGE(had_marker, "the pad wrote a marker before the launch");
    TEST_ASSERT_TRUE_MESSAGE(landed != 0, "the flight landed");
    TEST_ASSERT_FALSE_MESSAGE(marker_valid(), "after LANDED no power-up may recover against the old marker");
}

/* ── T3: triggers held for a duration ─────────────────────────────── */

/* Launch latency and T+0, from ignition, at four accelerations: the numbers
 * T3's latency test holds the held triggers to. */
typedef struct {
    double detect_ms, t0_ms;
} launch_times_t;

static launch_times_t launch_times(float g) {
    const flight_t f = {g, 3.0f, 20.0f, 0.0f};
    launch_times_t lt = {0, 0};
    int n = 0;
    for (uint32_t seed = 1; seed <= 5; seed++) {
        result_t r = fly(&f, seed, 10, 20000, false);
        if (r.launch_ms == 0)
            continue;
        lt.detect_ms += (double)r.launch_ms - (double)r.ignition_ms;
        lt.t0_ms += (double)ctx.launch_time - (double)r.ignition_ms;
        n++;
    }
    if (n) {
        lt.detect_ms /= n;
        lt.t0_ms /= n;
    }
    return lt;
}

/* Before T3, with T2's median in place: detection and T+0 after ignition. */
static const struct {
    float g;
    double detect_ms, t0_ms;
} BEFORE_T3[] = {{2.0f, 2260.0, 264.0}, {5.0f, 1560.0, 204.0}, {15.0f, 1020.0, 176.0}, {30.0f, 800.0, 156.0}};
#define BEFORE_T3_APOGEE_S 0.59

#define LAUNCH_HOLD_TEST_MS 100.0
#define APOGEE_HOLD_TEST_MS 60.0

void test_T3_pad_two_sample_glitch(void) {
    char launched[256] = "";
    for (unsigned i = 0; i < N_GLITCHES; i++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            boot_like_hardware(seed);
            uint32_t t = 0;
            run_to_pad(&t);
            uint32_t end = t + 5000u + 7u * seed;
            for (; t < end; t++)
                tick(t);
            mock_glitch_pa = GLITCHES[i];
            mock_glitch_samples = 2;
            end = t + 3000u;
            for (; t < end; t++)
                tick(t);
            if (ctx.current_state != PAD_IDLE) {
                char item[24];
                snprintf(item, sizeof(item), " %+ld(seed %u)", (long)(GLITCHES[i] / 1000), (unsigned)seed);
                strncat(launched, item, sizeof(launched) - 1 - strlen(launched));
            }
        }
    }
    if (launched[0])
        printf("  two glitches on the pad launched at (kPa):%s\n", launched);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", launched, "two glitches in a row must never declare a launch");
}

/* The coast sweep of T2, with two bad readings in a row. */
static void coast_glitch_sweep(int samples, char *early, size_t cap) {
    const flight_t flights[] = {
        {5.0f, 2.0f, 20.0f, 0.0f},
        {5.0f, 0.55f, 20.0f, 0.0f},
    };
    const int32_t sizes[] = {2000, 5000, 10000, 20000};
    for (unsigned fi = 0; fi < 2; fi++) {
        const flight_t *f = &flights[fi];
        float t_ap = f->burn_s + f->g_net * f->burn_s;
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            for (int k = 1; k <= 20; k++) {
                glitch.at_s = t_ap - 0.05f * (float)k;
                glitch.pa = sizes[i];
                glitch.samples = samples;
                result_t r = fly(f, 4, 3, 40000, false);
                if (r.apogee_ms != 0 && r.apogee_ms < r.apogee_true_ms) {
                    char item[64];
                    snprintf(item, sizeof(item), " [flight %u, +%ld kPa, %.2f s before: %ld ms early]", fi,
                             (long)(sizes[i] / 1000), 0.05 * k, (long)r.apogee_true_ms - (long)r.apogee_ms);
                    strncat(early, item, cap - 1 - strlen(early));
                }
            }
        }
    }
}

void test_T3_coast_two_sample_glitch(void) {
    char early[512] = "";
    coast_glitch_sweep(2, early, sizeof(early));
    if (early[0])
        printf("  early apogees:%s\n", early);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", early, "two glitches in coast must never declare apogee early");
}

void test_T3_latency(void) {
    for (unsigned i = 0; i < sizeof(BEFORE_T3) / sizeof(BEFORE_T3[0]); i++) {
        launch_times_t lt = launch_times(BEFORE_T3[i].g);
        char msg[128];
        snprintf(msg, sizeof(msg), "%.0f g: detected %.1f ms after ignition (was %.1f), T+0 %.1f ms (was %.1f)",
                 BEFORE_T3[i].g, lt.detect_ms, BEFORE_T3[i].detect_ms, lt.t0_ms, BEFORE_T3[i].t0_ms);
        TEST_ASSERT_TRUE_MESSAGE(lt.detect_ms <= BEFORE_T3[i].detect_ms + LAUNCH_HOLD_TEST_MS + 20.0, msg);
        TEST_ASSERT_TRUE_MESSAGE(lt.t0_ms == BEFORE_T3[i].t0_ms, msg);
    }
    const flight_t f = {5.0f, 2.0f, 20.0f, 0.0f};
    double sum = 0.0;
    int n = 0;
    for (uint32_t seed = 1; seed <= 20; seed++) {
        result_t r = fly(&f, seed, 3, 60000, false);
        if (r.apogee_ms == 0 || r.apogee_true_ms == 0)
            continue;
        sum += ((double)r.apogee_ms - (double)r.apogee_true_ms) / 1000.0;
        n++;
    }
    double mean = n ? sum / n : 99.0;
    char msg[96];
    snprintf(msg, sizeof(msg), "apogee %+.3f s after the true one (was %+.2f)", mean, BEFORE_T3_APOGEE_S);
    TEST_ASSERT_TRUE_MESSAGE(n == 20 && mean <= BEFORE_T3_APOGEE_S + APOGEE_HOLD_TEST_MS / 1000.0 + 0.02, msg);
}

/* At 100 Hz a 60 ms burst is six readings. A hold counted in samples -- five
 * samples is 100 ms at 50 Hz -- lets it through; one measured in time does
 * not. */
void test_T3_durations_not_counts(void) {
    boot_like_hardware(3);
    mock_sample_interval_ms = 10;
    uint32_t t = 0;
    run_to_pad(&t);
    uint32_t end = t + 5000u;
    for (; t < end; t++)
        tick(t);
    mock_glitch_pa = -20000;
    mock_glitch_samples = 6;
    end = t + 3000u;
    for (; t < end; t++)
        tick(t);
    TEST_ASSERT_EQUAL_MESSAGE(PAD_IDLE, ctx.current_state, "a 60 ms burst at 100 Hz must not launch");

    /* ...and a real flight at 100 Hz still launches and finds apogee. */
    const flight_t f = {5.0f, 2.0f, 20.0f, 0.0f};
    boot_like_hardware(4);
    mock_sample_interval_ms = 10;
    t = 0;
    uint32_t pad = run_to_pad(&t);
    uint32_t ign = pad + 3000u;
    truth_t tr = {0};
    uint32_t apogee_true = 0, apogee = 0;
    bool launched = false;
    for (; t < 40000u && apogee == 0; t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        float vb = tr.v;
        truth_step(&tr, &f, tf);
        if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f) {
            tr.apogee = true;
            apogee_true = t;
        }
        if (apogee_true && t == apogee_true - 300u) {
            mock_glitch_pa = 10000;
            mock_glitch_samples = 6;
        }
        mock_pressure.pressure_pa = isa_pa(tr.h);
        tick(t);
        launched |= ctx.current_state == ASCENT;
        if (ctx.apogee_detected)
            apogee = t;
    }
    TEST_ASSERT_TRUE_MESSAGE(launched, "a real flight at 100 Hz launches");
    TEST_ASSERT_TRUE_MESSAGE(apogee != 0 && apogee >= apogee_true, "and finds apogee after the true one");
}

/* N26: above 8 km the altitude clamp stopped the altitude, and a stopped
 * altitude read as a speed of zero -- apogee, while still climbing. About
 * 420 m/s at burnout; the true apogee is near 9.3 km AGL. */
void test_N26_apogee_above_8km(void) {
    const flight_t f = {30.0f, 1.43f, 20.0f, 0.0f};
    result_t r = fly(&f, 1, 3, 120000, false);
    char msg[96];
    snprintf(msg, sizeof(msg), "apogee %+ld ms from the true one", (long)r.apogee_ms - (long)r.apogee_true_ms);
    TEST_ASSERT_TRUE_MESSAGE(r.apogee_ms != 0 && r.apogee_ms >= r.apogee_true_ms, msg);
}

/* ── T7: the launch's ground pressure from before the rise ─────────── */

void test_T7_ground_error(void) {
    const float gs[] = {2.0f, 5.0f, 15.0f, 30.0f};
    char bad[256] = "";
    for (unsigned i = 0; i < 4; i++) {
        flight_t f = {gs[i], 3.0f, 20.0f, 0.0f};
        for (uint32_t seed = 1; seed <= 10; seed++) {
            result_t r = fly(&f, seed, 10, 20000, false);
            double err_m = (PAD_PA - (double)r.ground_frozen_pa) / PA_PER_M;
            if (r.launch_ms == 0 || fabs(err_m) > 0.1) {
                char item[48];
                snprintf(item, sizeof(item), " [%.0f g seed %u: %+.2f m]", gs[i], (unsigned)seed, err_m);
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    if (bad[0])
        printf("  frozen ground off by more than 0.1 m:%s\n", bad);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", bad, "the ground frozen at launch must be the pad's, within 0.1 m");
}

void test_T7_early_launch_degraded(void) {
    const flight_t f = {5.0f, 3.0f, 20.0f, 0.0f};
    fly(&f, 3, 0, 20000, false); /* ignition the moment PAD_IDLE begins */
    TEST_ASSERT_EQUAL(ASCENT, ctx.current_state == FALLING ? ASCENT : ctx.current_state);
    TEST_ASSERT_TRUE_MESSAGE(pp_ground_degraded(), "a launch with under a second of pad behind it is flagged");
    fly(&f, 3, 10, 20000, false);
    TEST_ASSERT_FALSE_MESSAGE(pp_ground_degraded(), "one with ten seconds is not");
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
    RUN_TEST(test_T2_pad_glitch_sweep);
    RUN_TEST(test_T2_coast_glitch);
    RUN_TEST(test_T2_calibration_glitch);
    RUN_TEST(test_T2_median_timing);
    RUN_TEST(test_T1_rejoins_descent);
    RUN_TEST(test_T1_rejoins_ascent);
    RUN_TEST(test_T1_still_board_stays_cold);
    RUN_TEST(test_T1_glitch_on_the_pad);
    RUN_TEST(test_T1_cold_reasons);
    RUN_TEST(test_T1_marker_invalid_after_landing);
    RUN_TEST(test_T3_pad_two_sample_glitch);
    RUN_TEST(test_T3_coast_two_sample_glitch);
    RUN_TEST(test_T3_latency);
    RUN_TEST(test_T3_durations_not_counts);
    RUN_TEST(test_N26_apogee_above_8km);
    RUN_TEST(test_T7_ground_error);
    RUN_TEST(test_T7_early_launch_degraded);
    return UNITY_END();
}
