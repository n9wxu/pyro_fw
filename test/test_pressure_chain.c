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
    return UNITY_END();
}
