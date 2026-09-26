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
#include <stdlib.h>
#include <string.h>
#include "../src/flight_states.h"
#include "../src/brownout.h"
#include "../src/buzzer.h"
#include "../src/hal.h"
#include "../src/telemetry_formatter.h"
#include "pressure_processing.h"
#include "board_harness.h"
#include "../sim/replay.h"
#include "../src/pressure_fit.h"
#include "../src/ms5607_driver.h"

extern reset_cause_t mock_reset_cause; /* test/hal_test.c */

void setUp(void) {}
void tearDown(void) {}

/* ── The truth ────────────────────────────────────────────────────── */

#define G 9.80665f
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

typedef struct {
    uint32_t ignition_ms, launch_ms, apogee_true_ms, apogee_ms, touchdown_ms, landed_ms;
    uint32_t apogee_sample_ms; /* the time of the sample apogee was declared on */
    uint32_t drop_ms;          /* when the true pressure first stood 1.0001 above its minimum */
    uint32_t pyro_ms[3];       /* when each channel fired, and the truth then */
    float pyro_v[3], pyro_h[3];
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

/* Options for the next fly(), cleared by each fly(). */
static struct {
    bool channels; /* the four settings below replace the defaults */
    uint8_t p1_mode, p2_mode;
    uint16_t p1_value, p2_value;
    bool stop_after_apogee; /* 2 s after the true apogee */
    void (*on_sample)(const truth_t *tr);
    uint32_t interval_ms; /* the sensor's sample interval; 0: the test HAL's 20 ms */
} fly_opts;

/* A flight from power-on to LANDED, or to until_ms. */
static result_t fly(const flight_t *f, uint32_t seed, uint32_t pad_s, uint32_t until_ms, bool stalls) {
    result_t r;
    memset(&r, 0, sizeof(r));
    boot_like_hardware(seed);
    mock_stall_model = stalls;
    if (fly_opts.interval_ms)
        mock_sample_interval_ms = fly_opts.interval_ms;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    r.ignition_ms = pad + pad_s * 1000u;
    if (fly_opts.channels) {
        ctx.config.pyro1_mode = fly_opts.p1_mode;
        ctx.config.pyro1_value = fly_opts.p1_value;
        ctx.config.pyro2_mode = fly_opts.p2_mode;
        ctx.config.pyro2_value = fly_opts.p2_value;
    }
    truth_t tr = {0};
    uint32_t last_sample = ctx.last_sample;
    bool glitched = false;
    float p_min = PAD_PA;
    /* The truth's speed by the millisecond, to set a sample's speed against
     * the truth at the sample's own time, not at the later tick that reads it. */
    static float v_at[512];
    for (; t < until_ms; t++) {
        float tf = ((float)t - (float)r.ignition_ms) / 1000.0f;
        if (glitch.samples > 0 && !glitched && tf >= glitch.at_s) {
            mock_glitch_pa = glitch.pa;
            mock_glitch_samples = glitch.samples;
            glitched = true;
        }
        float v_before = tr.v;
        truth_step(&tr, f, tf);
        v_at[t & 511u] = tr.v;
        if (!tr.apogee && tf > f->burn_s && v_before > 0.0f && tr.v <= 0.0f) {
            tr.apogee = true;
            r.apogee_true_ms = t;
        }
        if (tr.down && r.touchdown_ms == 0)
            r.touchdown_ms = t;
        float p = isa_pa(tr.h);
        if (!tr.apogee || p < p_min)
            p_min = p;
        else if (r.drop_ms == 0 && p >= 1.0001f * p_min)
            r.drop_ms = t;
        mock_pressure.pressure_pa = p;
        tick(t);
        const bool fired[3] = {false, ctx.pyro1_fired, ctx.pyro2_fired};
        for (int ch = 1; ch <= 2; ch++) {
            if (fired[ch] && r.pyro_ms[ch] == 0) {
                r.pyro_ms[ch] = t;
                r.pyro_v[ch] = tr.v;
                r.pyro_h[ch] = tr.h;
            }
        }
        if (ctx.current_state == ASCENT && r.launch_ms == 0) {
            r.launch_ms = t;
            r.ground_frozen_pa = ctx.ground_pressure;
        }
        float v_sample = v_at[ctx.last_sample & 511u];
        if (ctx.current_state == ASCENT && ctx.last_sample != last_sample && t - ctx.last_sample < 512u &&
            v_sample >= 90.0f && v_sample <= 110.0f) {
            double e = fabs(ctx.vertical_speed_cms / 100.0 - v_sample);
            r.speed_err_sq += e * e;
            r.speed_err_n++;
            if (e > r.speed_err_max)
                r.speed_err_max = e;
        }
        if (fly_opts.on_sample && ctx.last_sample != last_sample)
            fly_opts.on_sample(&tr);
        last_sample = ctx.last_sample;
        if (ctx.apogee_detected && r.apogee_ms == 0) {
            r.apogee_ms = t;
            r.apogee_sample_ms = ctx.last_sample;
        }
        if (ctx.current_state == LANDED) {
            r.landed_ms = t;
            break;
        }
        if (fly_opts.stop_after_apogee && r.apogee_true_ms != 0 && t > r.apogee_true_ms + 2000u)
            break;
    }
    memset(&glitch, 0, sizeof(glitch));
    memset(&fly_opts, 0, sizeof(fly_opts));
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
typedef struct {
    double mean_s, worst_s;
    int landed, never;
} touchdown_t;

static touchdown_t touchdown_to_landed(float land_m) {
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
    touchdown_t td = {n ? sum / n : 0.0, worst, n, never};
    return td;
}

void test_T0_baseline_touchdown_to_landed(void) {
    (void)touchdown_to_landed(0.0f);
    (void)touchdown_to_landed(5.0f);
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
    pad_marker_fill(&m, (int32_t)PAD_PA, 1200u);
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
 * the stage costs one sample of latency. Delivered with the middle sample's
 * own time, the times come out exactly as the ramp without the spike gives
 * them. The spike itself never reaches the filter: in the two windows that
 * hold it, the median is a neighbouring reading instead, one ramp step (10 Pa)
 * away, which moves the filtered altitude by centimetres, not the 400 m the
 * spike is worth. */
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
        TEST_ASSERT_INT32_WITHIN_MESSAGE(10, clean[i].alt, spiked[i].alt, "the spike reached the filter");
    }
}

/* ── T1: brownout recovery from real samples (N23, N25) ───────────── */

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

/* Before T3, with T2's median in place: detection after ignition. */
static const struct {
    float g;
    double detect_ms;
} BEFORE_T3[] = {{2.0f, 2260.0}, {5.0f, 1560.0}, {15.0f, 1020.0}, {30.0f, 800.0}};
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
        /* T+0 is the first half metre, which the truth reaches here. */
        double true_rise_ms = 1000.0 * sqrt(2.0 * 0.5 / (BEFORE_T3[i].g * G));
        char msg[128];
        snprintf(msg, sizeof(msg), "%.0f g: detected %.1f ms after ignition (was %.1f), T+0 %.1f ms (truth %.1f)",
                 BEFORE_T3[i].g, lt.detect_ms, BEFORE_T3[i].detect_ms, lt.t0_ms, true_rise_ms);
        TEST_ASSERT_TRUE_MESSAGE(lt.detect_ms <= BEFORE_T3[i].detect_ms + LAUNCH_HOLD_TEST_MS + 20.0, msg);
        TEST_ASSERT_TRUE_MESSAGE(lt.t0_ms >= true_rise_ms && lt.t0_ms <= true_rise_ms + 40.0, msg);
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

/* ── T4: fractional precision in the filter ───────────────────────── */

/* 60 s on the pad: the filtered height's spread, in pascals, and the pad
 * speed's, as the detectors read them. */
static void pad_quiet(double *height_rms_pa, double *speed_rms_ms) {
    boot_like_hardware(12);
    uint32_t t = 0;
    run_to_pad(&t);
    for (uint32_t end = t + 5000u; t < end; t++)
        tick(t);
    double sum = 0.0, sq = 0.0, vsq = 0.0;
    int n = 0;
    uint32_t last = ctx.last_sample;
    for (uint32_t end = t + 60000u; t < end; t++) {
        tick(t);
        if (ctx.last_sample != last) {
            double pa = ctx.last_height * (PA_PER_M / 100.0);
            sum += pa;
            sq += pa * pa;
            double v = ctx.pad_speed_cms / 100.0;
            vsq += v * v;
            n++;
            last = ctx.last_sample;
        }
    }
    double mean = sum / n;
    *height_rms_pa = sqrt(sq / n - mean * mean);
    *speed_rms_ms = sqrt(vsq / n);
}

void test_T4_filter_noise(void) {
    double h, v;
    pad_quiet(&h, &v);
    printf("  filtered pressure on the pad: %.3f Pa RMS\n", h);
    TEST_ASSERT_TRUE_MESSAGE(h <= 0.25, "the filter must pass no more than 0.25 Pa of 1.2 Pa noise");
}

void test_T4_pad_speed(void) {
    double h, v;
    pad_quiet(&h, &v);
    printf("  pad speed: %.2f m/s RMS\n", v);
    TEST_ASSERT_TRUE_MESSAGE(v <= 0.3, "speed noise on the pad must be 0.3 m/s RMS or less");
}

void test_T4_touchdown(void) {
    const float sites[] = {0.0f, 5.0f};
    for (int i = 0; i < 2; i++) {
        touchdown_t td = touchdown_to_landed(sites[i]);
        char msg[96];
        snprintf(msg, sizeof(msg), "%.0f m above the pad: %d of 20 landed, worst %.1f s", sites[i], td.landed,
                 td.worst_s);
        TEST_ASSERT_TRUE_MESSAGE(td.landed == 20 && td.worst_s <= 3.0, msg);
    }
}

/* ── T6: the ground tracker recovers from a step (N9) ─────────────── */

#define RESEED_TEST_MS 5000u

/* A step on the pad: the board carried to a pad higher or lower than the
 * prep table. Since T4 the filter moves smoothly enough through a small step
 * for the 5 s mean to creep after it; past the 50 Pa gate's reach the mean
 * locks out (N9) unless it re-seeds. Either way the reference must reach the
 * new level, and a re-seed must rewrite the marker. */
static void step_on_pad(float step_pa, char *bad, size_t cap) {
    boot_like_hardware(21);
    uint32_t t = 0;
    run_to_pad(&t);
    for (uint32_t end = t + 12000u; t < end; t++)
        tick(t);
    float level = 101325.0f + step_pa;
    mock_pressure.pressure_pa = level;
    uint32_t step_at = t, reseeded_at = 0;
    for (uint32_t end = t + 30000u; t < end; t++) {
        tick(t);
        if (reseeded_at == 0 && pp_ground_reseeds() > 0)
            reseeded_at = t;
    }
    int32_t off = pp_ground_pressure() - ((int32_t)level - 1);
    pad_marker_t m;
    int n = hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
    bool marker_ok =
        n == (int)sizeof(m) && pad_marker_valid(&m) && abs(m.ground_pressure_pa - ((int32_t)level - 1)) <= 3;
    bool ok =
        off >= -2 && off <= 2 && (reseeded_at == 0 || (reseeded_at - step_at <= RESEED_TEST_MS + 1500u && marker_ok));
    if (!ok) {
        char item[112];
        snprintf(item, sizeof(item), " [%+.0f Pa: ground %+ld Pa off, re-seeded %s, marker %s]", step_pa, (long)off,
                 reseeded_at ? "yes" : "no", marker_ok ? "new" : "stale");
        strncat(bad, item, cap - 1 - strlen(bad));
    }
}

void test_T6_step_reseeds(void) {
    const float steps[] = {-60.0f, -100.0f, -150.0f, -300.0f, 60.0f, 100.0f, 150.0f, 300.0f};
    char bad[768] = "";
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
        step_on_pad(steps[i], bad, sizeof(bad));
    if (bad[0])
        printf("  steps the ground did not recover from:%s\n", bad);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", bad, "after a step the ground must reach the new level");
}

/* guard: nothing that is not a step re-seeds. */
void test_T6_launch_never_reseeds(void) {
    const float gs[] = {2.0f, 5.0f, 15.0f, 30.0f};
    for (unsigned i = 0; i < 4; i++) {
        const flight_t f = {gs[i], 3.0f, 20.0f, 0.0f};
        result_t r = fly(&f, 5, 10, 60000, false);
        TEST_ASSERT_TRUE(r.launch_ms != 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pp_ground_reseeds(), "a launch is not a step on the pad");
    }
}

/* On odd sample times and even ones: the loop starting at 0 or at 2. */
void test_T6_gusts_never_reseed(void) {
    const float gusts[] = {20.0f, 40.0f, 60.0f, 80.0f, -80.0f};
    for (unsigned i = 0; i < 10; i++) {
        boot_like_hardware(22);
        uint32_t t = i < 5 ? 0u : 2u;
        run_to_pad(&t);
        for (uint32_t end = t + 6000u; t < end; t++)
            tick(t);
        for (int g = 0; g < 5; g++) {
            mock_pressure.pressure_pa = 101325.0f + gusts[i % 5];
            for (uint32_t end = t + RESEED_TEST_MS - 1000u; t < end; t++)
                tick(t);
            mock_pressure.pressure_pa = 101325.0f;
            for (uint32_t end = t + 3000u; t < end; t++)
                tick(t);
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pp_ground_reseeds(), "a gust shorter than the wait is not a step");
        TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    }
}

void test_T6_drift(void) {
    boot_like_hardware(23);
    uint32_t t = 0;
    run_to_pad(&t);
    uint32_t start = t;
    double worst = 0.0;
    for (uint32_t end = t + 3600000u; t < end; t++) {
        /* 2 hPa an hour, falling. */
        double truth = 101325.0 - 200.0 * (double)(t - start) / 3600000.0;
        mock_pressure.pressure_pa = (float)truth;
        tick(t);
        if (t % 1000u == 0 && t - start > 10000u) {
            double err = fabs((double)pp_ground_pressure() - truth);
            if (err > worst)
                worst = err;
        }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "worst ground error %.1f Pa", worst);
    TEST_ASSERT_TRUE_MESSAGE(worst <= PA_PER_M, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pp_ground_reseeds(), "drift is tracked, not re-seeded");
}

/* ── Section 5 ────────────────────────────────────────────────────── */

/* N18: LANDED logs one row a second, for telemetry and the ring's export. */
void test_N18_landed_logs_once_a_second(void) {
    const flight_t f = {5.0f, 1.0f, 5.0f, 0.0f};
    boot_like_hardware(31);
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    uint32_t ign = pad + 3000u, landed = 0;
    truth_t tr = {0};
    for (; t < 400000u; t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        float vb = tr.v;
        truth_step(&tr, &f, tf);
        if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f)
            tr.apogee = true;
        mock_pressure.pressure_pa = isa_pa(tr.h);
        tick(t);
        if (ctx.current_state == LANDED && landed == 0)
            landed = t;
        if (landed && t >= landed + 10000u)
            break;
    }
    TEST_ASSERT_TRUE_MESSAGE(landed != 0, "the flight landed");
    int rows = 0;
    for (int i = 0; i < FLIGHT_BUF_SIZE; i++)
        if (ctx.flight_buffer[i].state == LANDED && ctx.flight_buffer[i].event == EVT_NONE)
            rows++;
    char msg[64];
    snprintf(msg, sizeof(msg), "%d LANDED rows in 10 s", rows);
    TEST_ASSERT_TRUE_MESSAGE(rows >= 9 && rows <= 11, msg);
}

/* ── T11: every sample stamped at its conversion ───────────────────── */

/* Speed error at 90-110 m/s, as the baseline measures it. */
static double worst_speed_error(bool stalls) {
    const flight_t f = {15.0f, 1.2f, 20.0f, 0.0f};
    double worst = 0.0;
    for (uint32_t seed = 1; seed <= 10; seed++) {
        result_t r = fly(&f, seed, 3, 30000, stalls);
        if (r.speed_err_max > worst)
            worst = r.speed_err_max;
    }
    return worst;
}

/* The HAL's stamping, as the test HAL models it: with stalls, a speed no worse
 * than without, within 10 %. */
void test_T11_stalls_change_nothing(void) {
    double calm = worst_speed_error(false), stalled = worst_speed_error(true);
    char msg[96];
    snprintf(msg, sizeof(msg), "worst speed error %.1f m/s calm, %.1f m/s through stalls", calm, stalled);
    TEST_ASSERT_TRUE_MESSAGE(stalled <= 1.1 * calm, msg);
}

/* The MS5607 is stamped at the middle of its D1 conversion, from the hardware
 * timer: not at D2's command, which overwrites conv_start_us, and not at the
 * loop's read. */
void test_T11_d1_stamp(void) {
    uint64_t d1 = 123456789ull;
    TEST_ASSERT_EQUAL_UINT64(d1 + 4500u, ms5607_sample_time_us(d1));
}

/* Decisions are functions of the samples: a loop clock that runs late, by an
 * amount that wanders, changes which sample decided nothing. */
static uint32_t lag_state;
static uint32_t wandering_lag(uint32_t t) {
    (void)t;
    lag_state = lag_state * 1103515245u + 12345u;
    static uint32_t lag = 0;
    uint32_t r = (lag_state >> 16) % 3u;
    if (r == 0 && lag > 0)
        lag--; /* at most one back per tick: the loop clock never runs backwards */
    else if (r == 2 && lag < 70)
        lag++;
    return lag;
}

typedef struct {
    uint32_t at[12]; /* the sample time of each decision, in order */
    int n;
    uint32_t row_err_max; /* logged row time against its sample's */
} decisions_t;

static decisions_t decisions(bool lag) {
    const flight_t f = {5.0f, 2.0f, 20.0f, 0.0f};
    decisions_t d;
    memset(&d, 0, sizeof(d));
    boot_like_hardware(41);
    lag_state = 7;
    loop_lag_ms = lag ? wandering_lag : NULL;
    uint32_t t = 0;
    run_to_pad(&t);
    /* Ignition at a fixed sample time: the boot's own timers run on the loop
     * clock, so a lagged loop reaches PAD_IDLE sooner. */
    uint32_t ign = 12000u;
    truth_t tr = {0};
    flight_state_t last_state = ctx.current_state;
    int fires = 0;
    for (; t < 300000u && d.n < 12; t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        float vb = tr.v;
        truth_step(&tr, &f, tf);
        if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f)
            tr.apogee = true;
        mock_pressure.pressure_pa = isa_pa(tr.h);
        uint16_t head = ctx.buf_head;
        tick(t);
        if (ctx.current_state != last_state || mock_pyro.fire_count != fires) {
            d.at[d.n++] = ctx.last_sample;
            last_state = ctx.current_state;
            /* The igniter burns through, as a real one does. One that did
             * not would be retried after a grace on the loop clock, by design
             * [PYR-REFIRE-01], and that retry is not a decision on samples. */
            if (mock_pyro.fire_count != fires) {
                bool *good = mock_pyro.last_fire_channel == 1 ? &mock_pyro.p1_good : &mock_pyro.p2_good;
                bool *open = mock_pyro.last_fire_channel == 1 ? &mock_pyro.p1_open : &mock_pyro.p2_open;
                *good = false;
                *open = true;
            }
            fires = mock_pyro.fire_count;
        }
        /* A row added this tick belongs to the sample just taken. */
        if (ctx.buf_head != head && ctx.launch_time != 0) {
            const flight_sample_t *row = &ctx.flight_buffer[(ctx.buf_head + FLIGHT_BUF_SIZE - 1) % FLIGHT_BUF_SIZE];
            if (row->event == EVT_NONE) {
                uint32_t want = ctx.last_sample - ctx.launch_time;
                uint32_t err = row->time_ms > want ? row->time_ms - want : want - row->time_ms;
                if (err > d.row_err_max)
                    d.row_err_max = err;
            }
        }
        if (ctx.current_state == LANDED)
            break;
    }
    loop_lag_ms = NULL;
    return d;
}

void test_T11_loop_clock_independent(void) {
    decisions_t calm = decisions(false), lagged = decisions(true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(calm.n, lagged.n, "the same decisions, lag or not");
    for (int i = 0; i < calm.n; i++) {
        char msg[80];
        snprintf(msg, sizeof(msg), "decision %d: sample %u calm, %u lagged", i, (unsigned)calm.at[i],
                 (unsigned)lagged.at[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(calm.at[i], lagged.at[i], msg);
    }
}

void test_T11_log_rows_at_sample_time(void) {
    decisions_t lagged = decisions(true);
    char msg[64];
    snprintf(msg, sizeof(msg), "a row up to %u ms from its sample's time", (unsigned)lagged.row_err_max);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lagged.row_err_max, msg);
}

/* The landing hold is a full second of stillness in sample time, whatever
 * the parity of the sample that began it. Starting the loop at 0 puts every
 * sample on an odd millisecond, at 2 on an even one. */
void test_T11_landing_holds_a_second(void) {
    const flight_t f = {5.0f, 1.0f, 5.0f, 0.0f};
    char bad[256] = "";
    for (uint32_t seed = 1; seed <= 20; seed++) {
        boot_like_hardware(seed);
        power.landing_timeout = 0;
        uint32_t t = (seed & 1u) ? 0u : 2u;
        uint32_t pad = run_to_pad(&t);
        uint32_t ign = pad + 3000u + seed;
        truth_t tr = {0};
        for (; t < 400000u; t++) {
            float tf = ((float)t - (float)ign) / 1000.0f;
            float vb = tr.v;
            truth_step(&tr, &f, tf);
            if (!tr.apogee && tf > f.burn_s && vb > 0.0f && tr.v <= 0.0f)
                tr.apogee = true;
            mock_pressure.pressure_pa = isa_pa(tr.h);
            tick(t);
            if (ctx.current_state == LANDED)
                break;
        }
        uint32_t held = ctx.last_sample + 1u - ctx.landing_stable_since;
        if (ctx.current_state != LANDED || held < 1000u) {
            char item[40];
            snprintf(item, sizeof(item), " [seed %u: held %d ms]", (unsigned)seed, (int)held);
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    if (bad[0])
        printf("  landings short of the hold:%s\n", bad);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", bad, "LANDED needs a full second of stillness");
}

/* The rejection clock starts at zero on the first rejected sample, whatever
 * the parity of its time. */
void test_T6_rejecting_starts_at_zero(void) {
    for (uint32_t t0 = 1000; t0 <= 1001; t0++) {
        pp_init();
        pp_test_prime(101325);
        uint32_t t = t0;
        for (int i = 0; i < 20; i++, t += 20)
            pp_feed(101325, t);
        TEST_ASSERT_EQUAL_UINT32(0, pp_ground_rejecting_ms(t));
        /* Far past the gate, for long enough that the median passes it. */
        uint32_t bad_from = t;
        for (int i = 0; i < 3; i++, t += 20)
            pp_feed(101325 - 5000, t);
        uint32_t first = 0;
        altitude_sample_t a;
        while (pp_read(&a))
            if (first == 0 && a.timestamp_ms >= bad_from)
                first = a.timestamp_ms;
        TEST_ASSERT_EQUAL_UINT32(bad_from, first);
        uint32_t r = pp_ground_rejecting_ms(first);
        char msg[64];
        snprintf(msg, sizeof(msg), "t0 %u: rejecting for %u ms on its first sample", (unsigned)t0, (unsigned)r);
        TEST_ASSERT_TRUE_MESSAGE(r < 100u, msg);
    }
}

/* ── T8: raw readings in the log, and replay ──────────────────────── */

static char logbuf[65536];

static int flown_log(void) {
    const flight_t f = {5.0f, 1.0f, 20.0f, 0.0f}; /* apogee 147 m: the log fits the test HAL's file */
    result_t r = fly(&f, 51, 3, 120000, false);
    TEST_ASSERT_TRUE_MESSAGE(r.landed_ms != 0, "the flight landed and closed its log");
    int n = hal_fs_read_file("flight_log.csv", logbuf, (int)sizeof(logbuf) - 1);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "the flight wrote a log");
    logbuf[n] = '\0';
    return n;
}

void test_T8_columns(void) {
    flown_log();
    const char *cols = "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event\n";
    const char *hdr = strstr(logbuf, cols);
    TEST_ASSERT_NOT_NULL_MESSAGE(hdr, "the log names raw_pa and temp_c");
    int rows = 0, bad = 0;
    for (const char *p = hdr + strlen(cols); *p; p = strchr(p, '\n') + 1) {
        char line[128];
        size_t len = strcspn(p, "\n");
        if (len >= sizeof(line))
            break;
        memcpy(line, p, len);
        line[len] = '\0';
        int fields = 1;
        for (char *c = line; *c; c++)
            fields += *c == ',';
        long raw = 0;
        char ev[32] = "";
        if (sscanf(line, "%*[^,],%*[^,],%*[^,],%*[^,],%*[^,],%ld,%*[^,],%31s", &raw, ev) >= 1 && ev[0] == '\0') {
            rows++;
            bad += fields != 8 || raw < 1000 || raw > 120000;
        }
        if (!strchr(p, '\n'))
            break;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%d sample rows, %d without a plausible raw_pa", rows, bad);
    TEST_ASSERT_TRUE_MESSAGE(rows > 100 && bad == 0, msg);
}

/* The harness ends each pulse every tick; the replay's rows do the same. */
static void end_pulse(uint32_t now_ms) {
    (void)now_ms;
    mock_pyro.firing = false;
}

void test_T8_replay(void) {
    flown_log();
    replay_row_hook = end_pulse;
    replay_events_t logged, decided;
    TEST_ASSERT_TRUE(replay_logged_events(logbuf, &logged));
    TEST_ASSERT_TRUE_MESSAGE(replay_run(logbuf, &decided), "the log carries each sample's raw reading");
    replay_row_hook = NULL;
    char msg[192];
    snprintf(msg, sizeof(msg), "logged apogee %u pyro1 %u pyro2 %u landing %u; replayed %u %u %u %u; diverged at %u",
             (unsigned)logged.apogee_ms, (unsigned)logged.pyro1_ms, (unsigned)logged.pyro2_ms,
             (unsigned)logged.landing_ms, (unsigned)decided.apogee_ms, (unsigned)decided.pyro1_ms,
             (unsigned)decided.pyro2_ms, (unsigned)decided.landing_ms, (unsigned)decided.diverged_ms);
    TEST_ASSERT_TRUE_MESSAGE(logged.apogee_ms && logged.pyro1_ms && logged.pyro2_ms && logged.landing_ms, msg);
    const uint32_t a[] = {logged.apogee_ms, logged.pyro1_ms, logged.pyro2_ms, logged.landing_ms};
    const uint32_t b[] = {decided.apogee_ms, decided.pyro1_ms, decided.pyro2_ms, decided.landing_ms};
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_TRUE_MESSAGE(b[i] != 0 && (a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]) <= 20u, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, decided.diverged_ms, msg);
}

/* ── T5: one estimator, a fit to the pressure ─────────────────────── */

/* The endpoint of a least-squares quadratic, in double precision: the
 * reference the float fit is held to. */
static void ref_fit(const uint32_t *t_us, const int32_t *p, int n, double *p0, double *d1, double *d2) {
    double S[5] = {0}, T[3] = {0};
    for (int i = 0; i < n; i++) {
        double t = (double)(int32_t)(t_us[i] - t_us[n - 1]) / 1e6, y = (double)p[i], tk = 1.0;
        for (int k = 0; k < 5; k++) {
            S[k] += tk;
            if (k < 3)
                T[k] += y * tk;
            tk *= t;
        }
    }
    double A[3][4] = {{S[0], S[1], S[2], T[0]}, {S[1], S[2], S[3], T[1]}, {S[2], S[3], S[4], T[2]}};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            if (r != c) {
                double f = A[r][c] / A[c][c];
                for (int k = c; k < 4; k++)
                    A[r][k] -= f * A[c][k];
            }
    *p0 = A[0][3] / A[0][0];
    *d1 = A[1][3] / A[1][1];
    *d2 = 2.0 * A[2][3] / A[2][2];
}

/* 20 ms apart with up to 3 ms of jitter, a 60 ms stall, and a start that
 * carries the microsecond clock across its wrap (T11's wrap test). */
static int jittered_times(uint32_t *t_us, uint32_t start, uint32_t seed) {
    int n = 0;
    uint32_t t = start;
    for (int i = 0; i < 52 && n < 50; i++) {
        seed = seed * 1103515245u + 12345u;
        t += 20000u + (seed >> 16) % 6000u - 3000u;
        if (i == 20)
            continue; /* the stall's missing sample */
        t_us[n++] = t;
    }
    return n;
}

void test_T5_fit_reference(void) {
    uint32_t t_us[50];
    int32_t p[50];
    for (uint32_t trial = 0; trial < 200; trial++) {
        uint32_t start = trial < 100 ? 1000000u : 0xFFFFFFFFu - 400000u;
        int n = jittered_times(t_us, start, trial + 1);
        double b = -3000.0 + 30.0 * trial, c = 400.0 - 4.0 * trial;
        for (int i = 0; i < n; i++) {
            double t = (double)(int32_t)(t_us[i] - t_us[n - 1]) / 1e6;
            p[i] = (int32_t)lround(90000.0 + b * t + c * t * t);
        }
        double rp, r1, r2;
        ref_fit(t_us, p, n, &rp, &r1, &r2);
        pfit_t f = pfit_quadratic(t_us, p, n);
        char msg[128];
        snprintf(msg, sizeof(msg), "trial %u: p %.3f/%.3f pdot %.3f/%.3f pddot %.3f/%.3f", (unsigned)trial, f.p, rp,
                 f.pdot, r1, f.pddot, r2);
        TEST_ASSERT_TRUE_MESSAGE(f.valid && f.n == n, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(f.p - rp) < 0.05 && fabs(f.pdot - r1) < 0.1 && fabs(f.pddot - r2) < 0.5, msg);
    }
}

/* 50 samples 20 ms apart, noise sigma: the estimates' spread matches what
 * the fit's coefficients predict, sigma times the root of their squares. */
void test_T5_fit_noise(void) {
    uint32_t t_us[50];
    int32_t p[50];
    for (int i = 0; i < 50; i++)
        t_us[i] = 1000000u + 20000u * (uint32_t)i;
    /* The prediction, from the reference fit's response to a unit impulse. */
    double c1 = 0.0, c2 = 0.0;
    for (int k = 0; k < 50; k++) {
        for (int i = 0; i < 50; i++)
            p[i] = i == k ? 1000000 : 0;
        double rp, r1, r2;
        ref_fit(t_us, p, 50, &rp, &r1, &r2);
        c1 += (r1 / 1e6) * (r1 / 1e6);
        c2 += (r2 / 1e6) * (r2 / 1e6);
    }
    const double sigma = SENSOR_RMS_PA;
    double want1 = sigma * sqrt(c1), want2 = sigma * sqrt(c2);
    double sq1 = 0.0, sq2 = 0.0;
    uint32_t rng = 99;
    const int trials = 4000;
    for (int trial = 0; trial < trials; trial++) {
        for (int i = 0; i < 50; i++) {
            rng = rng * 1103515245u + 12345u;
            double u1 = ((rng >> 8) + 0.5) / 16777216.0;
            rng = rng * 1103515245u + 12345u;
            double u2 = ((rng >> 8) + 0.5) / 16777216.0;
            double g = sqrt(-2.0 * log(u1)) * cos(6.283185307 * u2);
            /* Scaled up so whole pascals do not add their own quantisation. */
            p[i] = (int32_t)lround(100000.0 + 100.0 * sigma * g);
        }
        pfit_t f = pfit_quadratic(t_us, p, 50);
        sq1 += (f.pdot / 100.0) * (f.pdot / 100.0);
        sq2 += (f.pddot / 100.0) * (f.pddot / 100.0);
    }
    double got1 = sqrt(sq1 / trials), got2 = sqrt(sq2 / trials);
    printf("  fit noise at 1.2 Pa: pdot %.2f Pa/s (predicted %.2f), pddot %.2f Pa/s^2 (predicted %.2f)\n", got1, want1,
           got2, want2);
    TEST_ASSERT_TRUE(fabs(got1 - want1) <= 0.1 * want1);
    TEST_ASSERT_TRUE(fabs(got2 - want2) <= 0.1 * want2);
}

void test_T5_clean(void) {
    int32_t series[200];
    uint32_t times[200];
    uint32_t rng = 5;
    for (int i = 0; i < 200; i++) {
        rng = rng * 1103515245u + 12345u;
        double u1 = ((rng >> 8) + 0.5) / 16777216.0;
        rng = rng * 1103515245u + 12345u;
        double u2 = ((rng >> 8) + 0.5) / 16777216.0;
        double g = sqrt(-2.0 * log(u1)) * cos(6.283185307 * u2);
        times[i] = 1000000u + 20000u * (uint32_t)i;
        series[i] = (int32_t)lround(100000.0 - 0.5 * i + SENSOR_RMS_PA * g) + (i >= 100 ? 12 : 0); /* a 10 sigma step */
    }
    int unclean_before = 0, clean_across = 0, unclean_after = 0;
    for (int end = 49; end < 200; end++) {
        pfit_t f = pfit_quadratic(times + end - 49, series + end - 49, 50);
        bool clean = pfit_clean(&f, SENSOR_RMS_PA);
        if (end < 100)
            unclean_before += !clean;
        else if (end - 49 < 100) /* the window holds samples from both sides */
            clean_across += clean;
        else
            unclean_after += !clean;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "unclean before the step %d, clean across it %d, unclean after %d", unclean_before,
             clean_across, unclean_after);
    TEST_ASSERT_TRUE_MESSAGE(unclean_before <= 2 && clean_across == 0 && unclean_after <= 2, msg);
}

/* ── T5: the detectors on the fit ─────────────────────────────────── */

/* σ is the fit's residual noise on the pad, floored at the datasheet's figure
 * and capped, and a brownout brings it back from the marker [FLT-BROWN-02]. */
void test_T5_sigma(void) {
    const struct {
        float noise, lo, hi;
    } cases[] = {
        /* The median leaves 0.67 of the noise: under the floor. */
        {SENSOR_RMS_PA, PP_SIGMA_FLOOR_PA, PP_SIGMA_FLOOR_PA},
        {3.0f, 1.7f, 2.4f},
        {12.0f, PP_SIGMA_CEIL_PA, PP_SIGMA_CEIL_PA},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        boot_like_hardware(4);
        mock_noise_rms_pa = cases[i].noise;
        uint32_t t = 0;
        run_to_pad(&t);
        for (uint32_t end = t + 12000u; t < end; t++)
            tick(t);
        float s = pp_sigma_pa();
        pad_marker_t m;
        memset(&m, 0, sizeof(m));
        (void)hal_fs_read_file(PAD_MARKER_PATH, (char *)&m, (int)sizeof(m));
        char msg[96];
        snprintf(msg, sizeof(msg), "%.1f Pa of noise: sigma %.2f Pa, marker %.2f Pa", (double)cases[i].noise,
                 (double)s, m.sigma_mpa / 1000.0);
        TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
        TEST_ASSERT_TRUE_MESSAGE(s >= cases[i].lo - 1e-3f && s <= cases[i].hi + 1e-3f, msg);
        TEST_ASSERT_TRUE_MESSAGE(pad_marker_valid(&m) && fabsf(m.sigma_mpa / 1000.0f - s) <= 0.1f * s, msg);
    }

    boot_like_hardware(5);
    pad_marker_t m;
    pad_marker_fill(&m, (int32_t)PAD_PA, 2500u);
    TEST_ASSERT_EQUAL(0, hal_fs_write_file(PAD_MARKER_PATH, (const char *)&m, (int)sizeof(m)));
    mock_reset_cause = RESET_POWER_EVENT;
    uint32_t t = 0;
    for (; t < 8000u && (booting() || t == 0); t++) {
        mock_pressure.pressure_pa = isa_pa(600.0f - 20.0f * (float)t / 1000.0f);
        tick(t);
    }
    TEST_ASSERT_EQUAL_MESSAGE(RECOVER_DESCENT, ctx.recovery, "recovered in descent");
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 2.5f, pp_sigma_pa());
}

/* [SNS-PRES-09] σ is the pad's, not the launch's: the first 50 Pa of a climb
 * pass the ground gate, and a fit through the ignition is no measure of the
 * sensor. */
static float sigma_before_ignition;
static uint32_t sigma_ignition_ms;

static void note_sigma(const truth_t *tr) {
    (void)tr;
    if (ctx.last_sample < sigma_ignition_ms)
        sigma_before_ignition = pp_sigma_pa();
}

void test_T5_sigma_ignores_the_launch(void) {
    const flight_t boosts[] = {{2.0f, 3.0f, 20.0f, 0.0f},
                               {5.0f, 2.0f, 20.0f, 0.0f},
                               {15.0f, 1.2f, 20.0f, 0.0f},
                               {30.0f, 1.0f, 20.0f, 0.0f}};
    char bad[256] = "";
    for (unsigned i = 0; i < sizeof(boosts) / sizeof(boosts[0]); i++) {
        const flight_t f = boosts[i];
        /* Noisier than the floor, so the pad's measurement shows. */
        boot_like_hardware(19);
        mock_noise_rms_pa = 3.0f;
        uint32_t t = 0;
        uint32_t pad = run_to_pad(&t);
        sigma_ignition_ms = pad + 12000u;
        truth_t tr = {0};
        uint32_t last = ctx.last_sample;
        for (; t < pad + 20000u; t++) {
            truth_step(&tr, &f, ((float)t - (float)sigma_ignition_ms) / 1000.0f);
            mock_pressure.pressure_pa = isa_pa(tr.h);
            tick(t);
            if (ctx.last_sample != last)
                note_sigma(&tr);
            last = ctx.last_sample;
        }
        /* Within what a five-second mean wanders in a second or so. */
        float after = pp_sigma_pa();
        if (ctx.current_state == PAD_IDLE || fabsf(after - sigma_before_ignition) > 0.05f * sigma_before_ignition) {
            char item[64];
            snprintf(item, sizeof(item), " %.0f g: %.2f Pa before, %.2f after;", (double)boosts[i].g_net,
                     (double)sigma_before_ignition, (double)after);
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-APO-01] Apogee on the fit: never before the true apogee, and as soon
 * after it as the 1.0001 drop allows. Timed by the decision's own sample,
 * against the moment the true pressure first stood 1.0001 above its minimum:
 * the pipeline's latency is not the estimator's. Half the flights have core0's
 * stalls. */
void test_T5_apogee(void) {
    const float gs[] = {3.0f, 5.0f, 10.0f, 15.0f};
    const int N = 1000;
    double sum = 0.0, lo = 1e9, hi = -1e9, after = 0.0;
    int early = 0, missed = 0, n = 0;
    for (int i = 0; i < N; i++) {
        float h = 100.0f * powf(90.0f, (float)i / (float)(N - 1)); /* 100 m to 9 km */
        float g = gs[i % 4];
        flight_t f = {g, sqrtf(2.0f * h / (g * G * (1.0f + g))), 20.0f, 0.0f};
        fly_opts.stop_after_apogee = true;
        result_t r = fly(&f, (uint32_t)i + 1u, 6, 200000u, (i & 1) != 0);
        if (r.apogee_ms == 0 || r.drop_ms == 0) {
            missed++;
            continue;
        }
        if ((int32_t)(r.apogee_sample_ms - r.apogee_true_ms) < 0)
            early++;
        double d = ((double)r.apogee_sample_ms - (double)r.drop_ms) / 1000.0;
        sum += d;
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
        after += ((double)r.apogee_sample_ms - (double)r.apogee_true_ms) / 1000.0;
        n++;
    }
    double mean = n ? sum / n : 0.0;
    printf("  apogee over %d flights, 100 m to 9 km: %+.3f s from the 1.0001 drop (%+.3f to %+.3f), %.2f s after "
           "the true apogee; %d early, %d missed\n",
           n, mean, lo, hi, n ? after / n : 0.0, early, missed);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, missed, "every flight's apogee is found");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, early, "never before the true apogee");
    TEST_ASSERT_TRUE_MESSAGE(fabs(mean) <= 0.1, "on average within 0.1 s of the 1.0001 drop");
}

/* [PYR-MODE-04, PYR-MODE-05, REV-05] A SPEED channel reads the fit's speed,
 * which does not lag; a DELAY channel counts from the fit's apogee, which the
 * fit dates back to where its rate crossed zero. */
void test_T5_speed_and_delay_triggers(void) {
    const flight_t f = {5.0f, 2.0f, 60.0f, 0.0f}; /* about 590 m; nearly free fall after apogee */
    double worst_v = 0.0, worst_d = 0.0;
    for (uint32_t seed = 1; seed <= 20; seed++) {
        fly_opts.channels = true;
        fly_opts.p1_mode = PYRO_MODE_DELAY;
        fly_opts.p1_value = 3;
        fly_opts.p2_mode = PYRO_MODE_SPEED;
        fly_opts.p2_value = 20;
        result_t r = fly(&f, seed, 6, 60000u, (seed & 1) != 0);
        TEST_ASSERT_TRUE_MESSAGE(r.pyro_ms[1] != 0 && r.pyro_ms[2] != 0, "both channels fired");
        double dv = fabs(-(double)r.pyro_v[2] - 20.0);
        double dd = fabs(((double)r.pyro_ms[1] - (double)r.apogee_true_ms) / 1000.0 - 3.0);
        worst_v = dv > worst_v ? dv : worst_v;
        worst_d = dd > worst_d ? dd : worst_d;
    }
    printf("  SPEED 20 m/s fired within %.2f m/s; DELAY 3 s within %.3f s of the true apogee plus 3 s\n", worst_v,
           worst_d);
    TEST_ASSERT_TRUE_MESSAGE(worst_v <= 1.0, "a SPEED channel fires within 1 m/s of its setting");
    TEST_ASSERT_TRUE_MESSAGE(worst_d <= 0.1, "a DELAY channel fires within 0.1 s of the true apogee plus its delay");
}

/* [DD-048] Two bad readings in a row under the drogue: the median passes one
 * or both, and every fit that holds them is unclean for a second. The main
 * waits them out: an early main is the flight's whole descent under it. */
void test_T5_descent_glitch(void) {
    const flight_t f = {5.0f, 2.0f, 20.0f, 0.0f}; /* about 590 m; the default main is at 300 m */
    const int32_t sizes[] = {2000, 5000, 10000, 18000};
    char bad[512] = "";
    float worst = 0.0f;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int k = 0; k < 8; k++) {
            float t_ap = f.burn_s + f.g_net * f.burn_s;
            float at_s = t_ap + 4.0f + 1.5f * (float)k; /* from 530 m down to 330 m */
            glitch.at_s = at_s;
            glitch.pa = sizes[i];
            glitch.samples = 2;
            result_t r = fly(&f, (uint32_t)k + 1u, 6, 60000u, false);
            float err = r.pyro_ms[2] ? r.pyro_h[2] - 300.0f : 1e6f;
            worst = fabsf(err) > worst ? fabsf(err) : worst;
            if (fabsf(err) > 8.0f) {
                char item[48];
                snprintf(item, sizeof(item), " +%ld kPa at %.1f s: %+.0f m;", (long)(sizes[i] / 1000),
                         (double)at_s, (double)err);
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    printf("  two glitches under the drogue: main within %.1f m of 300 m%s%s\n", (double)worst, bad[0] ? "; wrong:" : "",
           bad);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [SNS-ALT-04] Below the pad the reported altitude stays at zero while the
 * speed goes on reading the truth. The reported altitude is filtered, and
 * trails a 5 m/s descent by 2.5 m: it is checked from 5 m down. */
static struct {
    double err;
    int n, unclamped;
} below;

static void note_below(const truth_t *tr) {
    if (ctx.current_state == LANDED || tr->down || tr->h > -5.0f || tr->v >= 0.0f)
        return;
    below.err += ctx.vertical_speed_cms / 100.0 - tr->v;
    below.n++;
    if (ctx.last_altitude != 0)
        below.unclamped++;
}

void test_T5_through_the_clamp(void) {
    const flight_t f = {5.0f, 1.0f, 5.0f, -20.0f};
    memset(&below, 0, sizeof(below));
    for (uint32_t seed = 1; seed <= 10; seed++) {
        fly_opts.on_sample = note_below;
        (void)fly(&f, seed, 6, 200000u, false);
    }
    double mean = below.n ? below.err / below.n : 0.0;
    printf("  below the pad: speed error %+.2f m/s mean over %d samples; altitude off zero on %d\n", mean, below.n,
           below.unclamped);
    TEST_ASSERT_TRUE(below.n > 300);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, below.unclamped, "the reported altitude stays clamped at zero");
    TEST_ASSERT_TRUE_MESSAGE(fabs(mean) <= 0.3, "the speed stays unbiased through the clamp");
}

/* under_thrust from the fit's acceleration: on through the burn, off through
 * the coast, one change between. A one-second fit's acceleration crosses zero
 * 0.6-0.9 s after burnout, from 2 g to 30 g boosts (DD-048). */
#define THRUST_LAG_MAX_MS 1000u

static struct {
    bool launched, seen, first_on, prev;
    int changes;
    uint32_t last_on_ms;
} thrust;

static void note_thrust(const truth_t *tr) {
    (void)tr;
    if (ctx.current_state != ASCENT)
        return;
    if (!thrust.launched) {
        thrust.launched = true; /* the pad's sample that declared the launch */
        return;
    }
    bool on = ctx.under_thrust;
    if (!thrust.seen) {
        thrust.seen = true;
        thrust.first_on = on;
    } else if (on != thrust.prev) {
        thrust.changes++;
    }
    if (on)
        thrust.last_on_ms = ctx.last_sample;
    thrust.prev = on;
}

void test_T5_under_thrust(void) {
    const flight_t boosts[] = {{2.0f, 3.0f, 20.0f, 0.0f},
                               {5.0f, 2.0f, 20.0f, 0.0f},
                               {15.0f, 1.2f, 20.0f, 0.0f},
                               {30.0f, 1.0f, 20.0f, 0.0f}};
    char bad[256] = "";
    int worst_lag = 0;
    for (unsigned i = 0; i < sizeof(boosts) / sizeof(boosts[0]); i++) {
        for (uint32_t seed = 1; seed <= 5; seed++) {
            memset(&thrust, 0, sizeof(thrust));
            fly_opts.on_sample = note_thrust;
            fly_opts.stop_after_apogee = true;
            result_t r = fly(&boosts[i], seed, 6, 200000u, false);
            uint32_t burnout = r.ignition_ms + (uint32_t)(boosts[i].burn_s * 1000.0f);
            int lag = (int)thrust.last_on_ms - (int)burnout;
            worst_lag = lag > worst_lag ? lag : worst_lag;
            if (!thrust.first_on || thrust.changes != 1 || lag < -20 || lag > (int)THRUST_LAG_MAX_MS) {
                char item[64];
                snprintf(item, sizeof(item), " %.0fg/%u: %d changes, off %+d ms;", (double)boosts[i].g_net,
                         (unsigned)seed, thrust.changes, lag);
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    printf("  under_thrust ends at most %d ms after burnout%s%s\n", worst_lag, bad[0] ? "; wrong:" : "", bad);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* ── M1: the lockout's pieces that need a pad's truth ──────────────── */

/* [FLT-MACH-06] A recovered ascent has lost its speed history, so it starts
 * locked, flagged at the pressure it rejoined at. It must still release and
 * find apogee; a recovered descent is past all that. */
void test_M1_recovered_ascent_locked(void) {
    boot_like_hardware(2);
    write_marker((int32_t)PAD_PA);
    mock_reset_cause = RESET_POWER_EVENT;
    bool was_locked = false;
    int32_t p_flag = 0;
    float h = 300.0f, v = 50.0f, h_rejoin = 0.0f;
    for (uint32_t t = 0; t < 8000u; t++) {
        v -= G * 0.001f;
        h += v * 0.001f;
        mock_pressure.pressure_pa = isa_pa(h);
        bool was_booting = booting();
        tick(t);
        if (was_booting && !booting()) {
            was_locked = ctx.mach_lock;
            p_flag = ctx.p_flag_pa;
            h_rejoin = h;
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "rejoined locked %d, flagged at %ld Pa", was_locked, (long)p_flag);
    TEST_ASSERT_TRUE_MESSAGE(was_locked, msg);
    /* The level is the median of the newest 250 ms: at 50 m/s, 12 m below. */
    TEST_ASSERT_INT_WITHIN_MESSAGE(150, (int32_t)isa_pa(h_rejoin), p_flag, msg);

    air_boot_t a = boot_in_the_air(300.0f, 50.0f, 2, 60000);
    snprintf(msg, sizeof(msg), "drogue %ld ms after the true apogee", (long)a.fire_ms[1] - (long)a.apogee_ms);
    TEST_ASSERT_TRUE_MESSAGE(a.fire_ms[1] != 0 && a.fire_ms[1] >= a.apogee_ms && a.fire_ms[1] - a.apogee_ms < 1500,
                             msg);

    (void)boot_in_the_air(600.0f, -20.0f, 1, 3000);
    TEST_ASSERT_FALSE_MESSAGE(ctx.mach_lock, "a recovered descent is not locked");
}

/* [FLT-MACH-06] No channel arms below 0.9965 of the ground pressure, about
 * 30 m, and a flight that peaks at 25 m arms nothing. */
static struct {
    float armed_h;
    bool armed;
} arm_note;

static void note_arm(const truth_t *tr) {
    if (ctx.pyros_armed && !arm_note.armed) {
        arm_note.armed = true;
        arm_note.armed_h = tr->h;
    }
}

void test_M1_minimum_altitude_arm(void) {
    const float peaks[] = {25.0f, 40.0f, 60.0f, 150.0f};
    const float arm_h = 287.05f * 288.15f / G * -logf(0.9965f); /* 29.6 m on this pad */
    char bad[256] = "";
    for (unsigned i = 0; i < sizeof(peaks) / sizeof(peaks[0]); i++) {
        float g = 5.0f;
        flight_t f = {g, sqrtf(2.0f * peaks[i] / (g * G * (1.0f + g))), 20.0f, 0.0f};
        memset(&arm_note, 0, sizeof(arm_note));
        fly_opts.on_sample = note_arm;
        (void)fly(&f, 3, 6, 60000u, false);
        bool wrong = peaks[i] < arm_h ? arm_note.armed || mock_pyro.fire_count > 0
                                      : arm_note.armed && arm_note.armed_h < arm_h;
        if (wrong) {
            char item[64];
            snprintf(item, sizeof(item), " %.0f m peak: armed at %.1f m;", (double)peaks[i], (double)arm_note.armed_h);
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* ── T9: the chain at the MS5607's ~90 Hz ─────────────────────────── */

/* With D2 once in ten cycles the MS5607 gives a sample every 11 ms or so. */
#define T9_INTERVAL_MS 11u

/* [FLT-RATE-01] The idle time after a sample's conversions: none when they
 * fill the interval. Unsigned, it wrapped to 49 days. */
void test_T9_short_interval(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, ms5607_idle_ms(20u, 2u));
    TEST_ASSERT_EQUAL_UINT32(80u, ms5607_idle_ms(100u, 2u));
    TEST_ASSERT_EQUAL_UINT32(0u, ms5607_idle_ms(11u, 2u));
    TEST_ASSERT_EQUAL_UINT32(1u, ms5607_idle_ms(11u, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, ms5607_idle_ms(9u, 1u));
}

static double pad_speed_rms(uint32_t interval_ms) {
    boot_like_hardware(12);
    mock_sample_interval_ms = interval_ms;
    uint32_t t = 0;
    run_to_pad(&t);
    for (uint32_t end = t + 5000u; t < end; t++)
        tick(t);
    double sq = 0.0;
    int n = 0;
    uint32_t last = ctx.last_sample;
    for (uint32_t end = t + 60000u; t < end; t++) {
        tick(t);
        if (ctx.last_sample != last) {
            double v = ctx.pad_speed_cms / 100.0;
            sq += v * v;
            n++;
            last = ctx.last_sample;
        }
    }
    return sqrt(sq / n);
}

/* The same outcomes at ~90 Hz as at 50 Hz: the fit keeps its whole second,
 * so its noise falls as the root of the samples it holds. */
void test_T9_same_outcomes(void) {
    double v50 = pad_speed_rms(20u), v90 = pad_speed_rms(T9_INTERVAL_MS);
    printf("  pad speed: %.3f m/s RMS at 50 Hz, %.3f at 90 Hz\n", v50, v90);
    TEST_ASSERT_TRUE_MESSAGE(v90 <= 0.85 * v50, "a second of 90 Hz samples is quieter than one of 50 Hz");

    const float gs[] = {3.0f, 5.0f, 10.0f, 15.0f};
    double sum = 0.0;
    int early = 0, missed = 0, n = 0;
    for (int i = 0; i < 100; i++) {
        float h = 100.0f * powf(90.0f, (float)i / 99.0f);
        float g = gs[i % 4];
        flight_t f = {g, sqrtf(2.0f * h / (g * G * (1.0f + g))), 20.0f, 0.0f};
        fly_opts.stop_after_apogee = true;
        fly_opts.interval_ms = T9_INTERVAL_MS;
        result_t r = fly(&f, (uint32_t)i + 1u, 6, 200000u, false);
        if (r.apogee_ms == 0 || r.drop_ms == 0) {
            missed++;
            continue;
        }
        early += (int32_t)(r.apogee_sample_ms - r.apogee_true_ms) < 0;
        sum += ((double)r.apogee_sample_ms - (double)r.drop_ms) / 1000.0;
        n++;
    }
    double mean = n ? sum / n : 1.0;
    printf("  90 Hz apogee over %d flights: %+.3f s from the drop; %d early, %d missed\n", n, mean, early, missed);
    TEST_ASSERT_EQUAL_INT(0, missed);
    TEST_ASSERT_EQUAL_INT(0, early);
    TEST_ASSERT_TRUE(fabs(mean) <= 0.1);

    const flight_t f = {5.0f, 2.0f, 60.0f, 0.0f};
    double worst_v = 0.0, worst_d = 0.0;
    for (uint32_t seed = 1; seed <= 10; seed++) {
        fly_opts.channels = true;
        fly_opts.p1_mode = PYRO_MODE_DELAY;
        fly_opts.p1_value = 3;
        fly_opts.p2_mode = PYRO_MODE_SPEED;
        fly_opts.p2_value = 20;
        fly_opts.interval_ms = T9_INTERVAL_MS;
        result_t r = fly(&f, seed, 6, 60000u, false);
        TEST_ASSERT_TRUE(r.pyro_ms[1] != 0 && r.pyro_ms[2] != 0);
        double dv = fabs(-(double)r.pyro_v[2] - 20.0);
        double dd = fabs(((double)r.pyro_ms[1] - (double)r.apogee_true_ms) / 1000.0 - 3.0);
        worst_v = dv > worst_v ? dv : worst_v;
        worst_d = dd > worst_d ? dd : worst_d;
    }
    printf("  90 Hz: SPEED within %.2f m/s, DELAY within %.3f s\n", worst_v, worst_d);
    TEST_ASSERT_TRUE(worst_v <= 1.0);
    TEST_ASSERT_TRUE(worst_d <= 0.1);
}

/* ── N12: a drogue approaching its rate from below ─────────────────── */

/* [FLT-DESC-01] A drogue opened at apogee starts from rest and speeds up
 * toward its terminal rate, through the main's band on the way. It is a
 * drogue, and must never be reported as the main. No main is configured, so
 * nothing but the rate speaks for the phase. */
static struct {
    bool chute, drogue;
} phase_seen;

static void note_phase(const truth_t *tr) {
    if (tr->down)
        return;
    phase_seen.chute |= ctx.current_state == CHUTE_DESCENT;
    phase_seen.drogue |= ctx.current_state == DROGUE_DESCENT;
}

void test_N12_drogue_from_below(void) {
    const float rates[] = {12.0f, 15.0f, 20.0f, 25.0f};
    char bad[256] = "";
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        for (uint32_t seed = 1; seed <= 5; seed++) {
            const flight_t f = {5.0f, 2.0f, rates[i], 0.0f};
            memset(&phase_seen, 0, sizeof(phase_seen));
            fly_opts.channels = true;
            fly_opts.p1_mode = PYRO_MODE_DELAY;
            fly_opts.p1_value = 0;
            fly_opts.p2_mode = PYRO_MODE_NONE;
            fly_opts.p2_value = 0;
            fly_opts.on_sample = note_phase;
            (void)fly(&f, seed, 6, 200000u, false);
            if (phase_seen.chute || !phase_seen.drogue) {
                char item[64];
                snprintf(item, sizeof(item), " %.0f m/s seed %u: %s;", (double)rates[i], (unsigned)seed,
                         phase_seen.chute ? "reported as the main" : "never a drogue");
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* ── N7: the landing timeout under a main ─────────────────────────── */

/* [FLT-LAND-07] A main descends at 3-6 m/s, and a flight from 1 km spends
 * minutes under it: the 60 s timeout must wait for stillness, not declare
 * LANDED on "slower than 5 m/s" in the air. On the pad's level and 50 m above
 * it, past the 30 m the stillness test itself asks for, where only the
 * timeout can land the flight. */
void test_N7_no_landing_under_main(void) {
    const float sites[] = {0.0f, 50.0f};
    char bad[256] = "";
    for (unsigned i = 0; i < 2; i++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            const flight_t f = {5.0f, 3.0f, 5.0f, sites[i]}; /* about 1.3 km, then 5 m/s */
            result_t r = fly(&f, seed, 6, 400000u, false);
            float land = r.landed_ms && r.touchdown_ms ? ((float)r.landed_ms - (float)r.touchdown_ms) / 1000.0f : 1e6f;
            if (r.landed_ms == 0 || land < 0.0f || land > 3.0f) {
                char item[64];
                if (r.landed_ms && !r.touchdown_ms)
                    snprintf(item, sizeof(item), " %.0f m up, seed %u: LANDED in the air;", (double)sites[i],
                             (unsigned)seed);
                else
                    snprintf(item, sizeof(item), " %.0f m up, seed %u: LANDED %+.1f s from touchdown;",
                             (double)sites[i], (unsigned)seed, (double)land);
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
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
    RUN_TEST(test_T4_filter_noise);
    RUN_TEST(test_T4_pad_speed);
    RUN_TEST(test_T4_touchdown);
    RUN_TEST(test_T6_step_reseeds);
    RUN_TEST(test_T6_launch_never_reseeds);
    RUN_TEST(test_T6_gusts_never_reseed);
    RUN_TEST(test_T6_drift);
    RUN_TEST(test_T6_rejecting_starts_at_zero);
    RUN_TEST(test_N18_landed_logs_once_a_second);
    RUN_TEST(test_T11_stalls_change_nothing);
    RUN_TEST(test_T11_d1_stamp);
    RUN_TEST(test_T11_loop_clock_independent);
    RUN_TEST(test_T11_log_rows_at_sample_time);
    RUN_TEST(test_T11_landing_holds_a_second);
    RUN_TEST(test_T8_columns);
    RUN_TEST(test_T8_replay);
    RUN_TEST(test_T5_fit_reference);
    RUN_TEST(test_T5_fit_noise);
    RUN_TEST(test_T5_clean);
    RUN_TEST(test_T5_sigma);
    RUN_TEST(test_T5_sigma_ignores_the_launch);
    RUN_TEST(test_T5_apogee);
    RUN_TEST(test_T5_speed_and_delay_triggers);
    RUN_TEST(test_T5_descent_glitch);
    RUN_TEST(test_T5_through_the_clamp);
    RUN_TEST(test_T5_under_thrust);
    RUN_TEST(test_M1_recovered_ascent_locked);
    RUN_TEST(test_M1_minimum_altitude_arm);
    RUN_TEST(test_N12_drogue_from_below);
    RUN_TEST(test_N7_no_landing_under_main);
    RUN_TEST(test_T9_short_interval);
    RUN_TEST(test_T9_same_outcomes);
    return UNITY_END();
}
