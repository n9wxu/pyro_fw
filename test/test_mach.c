/*
 * The Mach lockout's test ground: supersonic flights from a cold sea-level pad
 * and a hot high one, with static ports that misreport while the rocket is
 * fast, ejection charges that pressurise the bay, and sensors that stop or
 * stick. The firmware flies each one through the board harness, and the
 * plant's truth says what really happened.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "mocks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/flight_states.h"
#include "../src/hal.h"
#include "pressure_processing.h"
#include "board_harness.h"
#include "mach_plant.h"
#include "../src/mach_lockout.h"

void setUp(void) {}
void tearDown(void) {}

/* ── The sites and the rockets ────────────────────────────────────── */

static const mp_site_t COLD = {10.0f, 0.0f};   /* 10 °C at sea level */
static const mp_site_t HOT = {45.0f, 2000.0f}; /* 45 °C at 2000 m */
/* The standard atmosphere's own pad, where the firmware's pressure altitude is
 * the true height: elsewhere an AGL setting is off by the air's temperature,
 * 1.8 % at the cold pad, whatever the estimator does. */
static const mp_site_t ISA = {15.0f, 0.0f};

typedef struct {
    const char *name;
    mp_rocket_t r;
} profile_t;

/* Tuned so each does what its name says at the cold pad; the hot pad's thin
 * air lets each go a little faster and higher. */
static const profile_t PROFILES[] = {
    {"subsonic", {1.0f, 0.10f, 141.0f, 1.5f, 0.0020f, 2.0f, 20.0f}},  /* Mach 0.50 */
    {"mid-Mach", {1.0f, 0.15f, 220.0f, 1.6f, 0.0020f, 2.0f, 20.0f}},  /* Mach 0.76 */
    {"draggy", {2.0f, 0.60f, 1709.0f, 1.5f, 0.0045f, 3.0f, 20.0f}},   /* Mach 1.50, 66 g */
    {"low-drag", {8.0f, 4.00f, 1216.0f, 5.0f, 0.0008f, 1.5f, 20.0f}}, /* Mach 1.60, 9.5 km */
    {"30 g", {1.0f, 0.30f, 395.0f, 1.1f, 0.0015f, 2.0f, 20.0f}},      /* Mach 0.93 at 0.96 s */
};
#define N_PROFILES (sizeof(PROFILES) / sizeof(PROFILES[0]))
enum { SUBSONIC, MID_MACH, DRAGGY, LOW_DRAG, BOOST_30G };

/* A persistent error for the whole supersonic period, stepping at Mach 1. */
static const mp_port_t PORT_READS_HIGH = {+1.0f, 0.02f, 0.05f, 0.03f};
static const mp_port_t PORT_READS_LOW = {-1.0f, 0.02f, 0.05f, 0.03f};
/* One that grows fast enough through the boost to make a climbing rocket
 * look as if it were falling. */
static const mp_port_t PORT_FAKES_DESCENT = {+1.0f, 0.06f, 0.14f, 0.05f};

/* ── A flight ─────────────────────────────────────────────────────── */

typedef struct {
    mp_port_t port;     /* all zero: clean ports */
    mp_charge_t charge; /* all zero: no pressure in the bay */
    float dropout_at_s, dropout_s;
    float stuck_at_s; /* 0: never */
    float stuck_s;    /* 0: for good */
    float glitch_at_s; /* mock_glitch_pa for glitch_n readings from here; 0: none */
    int32_t glitch_pa;
    int glitch_n;
    float swing_rms_pa; /* under a canopy, until touchdown */
    uint16_t main_m;    /* an AGL main at this height; 0: the default channels */
    float main_ms;      /* the main's descent rate; 0: the main changes nothing */
    bool to_landed;     /* on past touchdown to LANDED, with the landing timeout off */
    float coast_noise_pa;   /* ports this noisy from burnout: no fit is clean */
    float coast_noise_to_s; /* until this flight time; 0: until the true apogee */
    bool stop_after_apogee; /* 2 s after the true apogee */
    uint32_t interval_ms;   /* the sensor's sample interval; 0: the test HAL's 20 ms */
} conditions_t;

typedef struct {
    bool launched;
    bool drogue;
    float drogue_t, apogee_t, apogee_h, max_mach;
    bool locked; /* the Mach lockout's flag was ever set */
    float flag_t, flag_mach, flag_h;
    bool released; /* the lock let go on the signature of a coast */
    float release_t, release_mach;
    bool fallback;   /* the lock's fallback fired the drogue */
    float return_t;  /* the truth first back below the flag's pressure, after apogee */
    int32_t peak_cm; /* the reported peak */
    bool peak_lower_bound;
    bool main;
    float main_t, main_h;
    float touchdown_t, landed_t; /* 0: not reached */
} mach_result_t;

static mach_result_t fly_mach(const mp_rocket_t *r, const mp_site_t *s, const conditions_t *c, uint32_t seed,
                              float until_s) {
    mach_result_t res;
    memset(&res, 0, sizeof(res));
    boot_like_hardware(seed);
    if (c->interval_ms)
        mock_sample_interval_ms = c->interval_ms;
    if (c->to_landed)
        power.landing_timeout = 0;
    float pad_pa = mp_pad_pa(s);
    mock_pressure.pressure_pa = pad_pa;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    if (c->main_m) {
        ctx.config.pyro2_mode = PYRO_MODE_AGL;
        ctx.config.pyro2_value = c->main_m;
    }
    uint32_t ign = pad + 10000u;
    mp_rocket_t rk = *r;
    /* The plant's Mach by the millisecond: the flag is judged by the Mach at
     * its own sample's time, which is the data it used. */
    static float mach_at[1024], h_at[1024];
    mp_state_t st;
    mp_launch(&st);
    int fires = 0;
    float charge_at = -1.0f;
    bool glitched = false;
    for (; t < ign + (uint32_t)(until_s * 1000.0f); t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        if (tf >= 0.0f)
            mp_step(&st, s, &rk, 0.001f);
        mach_at[t & 1023u] = st.mach;
        h_at[t & 1023u] = st.h;
        float static_pa = mp_pressure_pa(s, st.h);
        float sensed = static_pa + mp_port_error_pa(&c->port, st.mach, static_pa);
        if (charge_at >= 0.0f)
            sensed += mp_charge_pa(&c->charge, tf - charge_at);
        mock_pressure.pressure_pa = sensed;
        bool out = c->dropout_s > 0.0f && tf >= c->dropout_at_s && tf < c->dropout_at_s + c->dropout_s;
        mock_pressure.sensor_type = out ? 0 : 2;
        mock_sensor_stuck = c->stuck_at_s > 0.0f && tf >= c->stuck_at_s && (c->stuck_s <= 0.0f || tf < c->stuck_at_s + c->stuck_s);
        if (c->glitch_n > 0 && tf >= c->glitch_at_s && !glitched) {
            mock_glitch_pa = c->glitch_pa;
            mock_glitch_samples = c->glitch_n;
            glitched = true;
        }
        if (c->swing_rms_pa > 0.0f)
            mock_noise_rms_pa = st.canopy && !st.landed ? c->swing_rms_pa : SENSOR_RMS_PA;
        if (c->coast_noise_pa > 0.0f) {
            bool noisy = tf > r->burn_s && !st.apogee && (c->coast_noise_to_s <= 0.0f || tf < c->coast_noise_to_s);
            mock_noise_rms_pa = noisy ? c->coast_noise_pa : SENSOR_RMS_PA;
        }
        tick(t);
        res.launched |= ctx.current_state == ASCENT;
        while (fires < mock_pyro.fire_count) {
            fires++;
            if (mock_pyro.last_fire_channel == 1 && !res.drogue) {
                res.drogue = true;
                res.drogue_t = tf;
                st.canopy = true;
                if (c->charge.peak_pa > 0.0f)
                    charge_at = tf;
            } else if (mock_pyro.last_fire_channel == 2 && !res.main) {
                res.main = true;
                res.main_t = tf;
                res.main_h = st.h;
                if (c->main_ms > 0.0f)
                    rk.canopy_ms = c->main_ms;
            }
        }
        if (ctx.mach_lock && !res.locked) {
            res.locked = true;
            bool recent = t - ctx.mach_flag_ms < 1024u;
            res.flag_t = recent ? ((float)ctx.mach_flag_ms - (float)ign) / 1000.0f : tf;
            res.flag_mach = recent ? mach_at[ctx.mach_flag_ms & 1023u] : st.mach;
            res.flag_h = recent ? h_at[ctx.mach_flag_ms & 1023u] : st.h;
        }
        if (res.locked && !ctx.mach_lock && !ctx.mach_fallback && !res.released) {
            res.released = true;
            res.release_t = tf;
            res.release_mach = st.mach;
        }
        res.fallback |= ctx.mach_fallback;
        if (res.locked && st.apogee && res.return_t == 0.0f && static_pa > (float)ctx.p_flag_pa)
            res.return_t = tf;
        if (c->to_landed && ctx.current_state == LANDED) {
            res.landed_t = tf;
            break;
        }
        if (c->stop_after_apogee && st.apogee && tf > st.apogee_t + 2.0f)
            break;
        if (st.landed && res.touchdown_t == 0.0f)
            res.touchdown_t = tf;
        if (st.landed && (!c->to_landed || tf - res.touchdown_t > 10.0f))
            break;
    }
    res.apogee_t = st.apogee_t;
    res.apogee_h = st.apogee_h;
    res.max_mach = st.max_mach;
    res.peak_cm = ctx.max_altitude;
    res.peak_lower_bound = ctx.peak_lower_bound;
    return res;
}

/* ── M0: the plant itself ─────────────────────────────────────────── */

/* The closed form against the hydrostatic equation integrated a metre at a
 * time, through the tropopause. */
void test_M0_atmosphere(void) {
    const mp_site_t sites[] = {COLD, HOT};
    for (int i = 0; i < 2; i++) {
        const mp_site_t *s = &sites[i];
        double p = mp_pad_pa(s);
        for (int h = 0; h < 12000 - (int)s->elev_m; h++) {
            if (h % 500 == 0) {
                double want = p, got = mp_pressure_pa(s, (float)h);
                char msg[80];
                snprintf(msg, sizeof(msg), "%.0f C pad, %d m: %.1f Pa against %.1f", s->temp_c, h, got, want);
                TEST_ASSERT_TRUE_MESSAGE(fabs(got - want) <= 0.001 * want, msg);
            }
            double tk = mp_temp_k(s, (float)h + 0.5f);
            p -= p * MP_G / (MP_R * tk);
        }
        TEST_ASSERT_FLOAT_WITHIN(0.01f, s->temp_c + 273.15f - 6.5f, mp_temp_k(s, 1000.0f));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, mp_temp_k(s, 11000.0f - s->elev_m), mp_temp_k(s, 12000.0f - s->elev_m));
    }
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 101325.0f, mp_pad_pa(&COLD));
    TEST_ASSERT_FLOAT_WITHIN(20.0f, 79495.0f, mp_pad_pa(&HOT));
}

void test_M0_mach(void) {
    mp_state_t st;
    mp_launch(&st);
    for (int i = 0; i < 3000; i++) {
        mp_step(&st, &HOT, &PROFILES[DRAGGY].r, 0.001f);
        if (i % 500 == 499) {
            float want = st.v / sqrtf(MP_GAMMA * MP_R * mp_temp_k(&HOT, st.h));
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, want, st.mach);
        }
    }
}

void test_M0_port_error(void) {
    const float p = 80000.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mp_port_error_pa(&PORT_READS_HIGH, 0.5f, p));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mp_port_error_pa(&PORT_READS_HIGH, 0.85f, p));
    /* Continuous from 0.85 to just under Mach 1... */
    float prev = 0.0f;
    for (float m = 0.86f; m < 0.999f; m += 0.01f) {
        float e = mp_port_error_pa(&PORT_READS_HIGH, m, p);
        TEST_ASSERT_TRUE(e > prev && e - prev < 0.01f * p);
        prev = e;
    }
    /* ...stepping at Mach 1, in both directions... */
    float below = mp_port_error_pa(&PORT_READS_HIGH, 0.9999f, p);
    float above = mp_port_error_pa(&PORT_READS_HIGH, 1.0f, p);
    float q = 0.5f * MP_GAMMA * p;
    TEST_ASSERT_FLOAT_WITHIN(0.002f * q, (0.05f - 0.02f) * q, above - below);
    TEST_ASSERT_EQUAL_FLOAT(mp_port_error_pa(&PORT_READS_HIGH, 1.3f, p), mp_port_error_pa(&PORT_READS_HIGH, -1.3f, p));
    /* ...and with the sign it is given. */
    TEST_ASSERT_EQUAL_FLOAT(-mp_port_error_pa(&PORT_READS_HIGH, 1.3f, p), mp_port_error_pa(&PORT_READS_LOW, 1.3f, p));

    /* The descent-faking error: through the boost the sensed pressure rises,
     * which is a climbing rocket reading as falling. */
    mp_state_t st;
    mp_launch(&st);
    float prev_sensed = 0.0f;
    bool faked = false, climbing = true;
    while (st.t < PROFILES[DRAGGY].r.burn_s) {
        mp_step(&st, &COLD, &PROFILES[DRAGGY].r, 0.001f);
        float sp = mp_pressure_pa(&COLD, st.h);
        float sensed = sp + mp_port_error_pa(&PORT_FAKES_DESCENT, st.mach, sp);
        if (prev_sensed > 0.0f && sensed > prev_sensed)
            faked = true;
        climbing &= st.v > 0.0f;
        prev_sensed = sensed;
    }
    TEST_ASSERT_TRUE_MESSAGE(climbing, "the rocket climbs through the whole boost");
    TEST_ASSERT_TRUE_MESSAGE(faked, "yet its sensed pressure rises: it reads as falling");
}

void test_M0_charge(void) {
    const mp_charge_t c = {3000.0f, 0.2f};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mp_charge_pa(&c, -0.1f));
    TEST_ASSERT_EQUAL_FLOAT(3000.0f, mp_charge_pa(&c, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 3000.0f / 2.71828f, mp_charge_pa(&c, 0.2f));
}

void test_M0_failures(void) {
    /* A dropout feeds nothing; a stuck sensor repeats itself to the pascal,
     * noise or not. */
    boot_like_hardware(1);
    uint32_t t = 0;
    run_to_pad(&t);
    uint32_t last = ctx.last_sample;
    mock_pressure.sensor_type = 0;
    for (uint32_t end = t + 500u; t < end; t++)
        tick(t);
    TEST_ASSERT_TRUE_MESSAGE(ctx.last_sample - last <= 60u, "no new sample during the dropout");
    mock_pressure.sensor_type = 2;
    for (uint32_t end = t + 100u; t < end; t++)
        tick(t);
    mock_sensor_stuck = true;
    int32_t first = 0;
    bool same = true;
    for (uint32_t end = t + 500u; t < end; t++) {
        tick(t);
        if (t % 20 == 0) {
            if (first == 0)
                first = pp_last_raw_pa();
            same &= pp_last_raw_pa() == first;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(same, "a stuck sensor repeats its reading exactly");
}

static void profile_reaches(unsigned i, const mp_site_t *s, float lo, float hi) {
    mp_state_t st;
    mp_launch(&st);
    float sup_from = -1.0f, sup_to = -1.0f;
    while (!st.apogee && st.t < 200.0f) {
        mp_step(&st, s, &PROFILES[i].r, 0.001f);
        if (fabsf(st.mach) >= 1.0f) {
            if (sup_from < 0.0f)
                sup_from = st.t;
            sup_to = st.t;
        }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%s from the %.0f C pad: Mach %.2f, apogee %.0f m", PROFILES[i].name, s->temp_c,
             st.max_mach, st.apogee_h);
    TEST_ASSERT_TRUE_MESSAGE(st.max_mach >= lo && st.max_mach <= hi, msg);
    if (i == LOW_DRAG) {
        snprintf(msg, sizeof(msg), "low-drag supersonic for %.1f s, apogee %.0f m", sup_to - sup_from, st.apogee_h);
        TEST_ASSERT_TRUE_MESSAGE(sup_to - sup_from >= 10.0f && st.apogee_h > 9000.0f && st.apogee_h < 11000.0f, msg);
    }
}

void test_M0_profiles(void) {
    profile_reaches(SUBSONIC, &COLD, 0.0f, 0.6f);
    profile_reaches(SUBSONIC, &HOT, 0.0f, 0.6f);
    profile_reaches(MID_MACH, &COLD, 0.65f, 0.85f);
    profile_reaches(MID_MACH, &HOT, 0.65f, 0.85f);
    profile_reaches(DRAGGY, &COLD, 1.45f, 1.55f);
    profile_reaches(DRAGGY, &HOT, 1.45f, 2.1f);
    profile_reaches(LOW_DRAG, &COLD, 1.5f, 1.7f);
    profile_reaches(LOW_DRAG, &HOT, 1.4f, 1.7f);
    profile_reaches(BOOST_30G, &COLD, 0.86f, 1.2f);
    const mp_rocket_t *b = &PROFILES[BOOST_30G].r;
    float net_g = (b->thrust_n / (b->dry_kg + b->prop_kg) - MP_G) / MP_G;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 30.0f, net_g);
}

/* ── M0: the report, today ────────────────────────────────────────── */

/* For each profile, pad and port: did the drogue fire, when against the true
 * apogee, and where the Mach gate let go. What M1 must change is read here. */
void test_M0_report(void) {
    const mp_site_t *sites[] = {&COLD, &HOT};
    const struct {
        const char *name;
        const mp_port_t *port;
    } ports[] = {{"clean", NULL},
                 {"reads high", &PORT_READS_HIGH},
                 {"reads low", &PORT_READS_LOW},
                 {"fakes descent", &PORT_FAKES_DESCENT}};
    printf("  %-9s %-5s %-13s %-9s %-17s %s\n", "profile", "pad", "ports", "max Mach", "drogue", "lock release");
    for (unsigned i = 0; i < N_PROFILES; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 4; pi++) {
                if (pi > 0 && i < DRAGGY && i != BOOST_30G)
                    continue; /* subsonic flights see no port error */
                conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi].port)
                    c.port = *ports[pi].port;
                mach_result_t r = fly_mach(&PROFILES[i].r, sites[si], &c, 7, 120.0f);
                TEST_ASSERT_TRUE_MESSAGE(r.launched, PROFILES[i].name);
                char drogue[32], release[64];
                if (r.drogue)
                    snprintf(drogue, sizeof(drogue), "%+.2f s at apogee", r.drogue_t - r.apogee_t);
                else
                    snprintf(drogue, sizeof(drogue), "never");
                if (!r.locked)
                    snprintf(release, sizeof(release), "never locked");
                else if (r.released)
                    snprintf(release, sizeof(release), "Mach %.2f, %.1f s before apogee", r.release_mach,
                             r.apogee_t - r.release_t);
                else if (r.fallback)
                    snprintf(release, sizeof(release), "fallback");
                else
                    snprintf(release, sizeof(release), "never released");
                printf("  %-9s %2.0f C  %-13s %-9.2f %-17s %s\n", PROFILES[i].name, sites[si]->temp_c, ports[pi].name,
                       r.max_mach, drogue, release);
            }
        }
    }
}

/* ── T5: the fit through a charge and a swinging canopy ───────────── */

static float plant_apogee(const mp_rocket_t *r, const mp_site_t *s) {
    mp_state_t st;
    mp_launch(&st);
    while (!st.apogee && st.t < 200.0f)
        mp_step(&st, s, r, 0.001f);
    return st.apogee_h;
}

#define MAIN_TOL_M 8.0f

/* The drogue's charge pressurises the bay at apogee: to the pressure layer the
 * rocket has dropped hundreds of metres in an instant. Pressure triggers wait
 * for a clean fit, up to 2 s, so the main 100 m below still fires where it is
 * set. */
void test_T5_ejection(void) {
    const mp_rocket_t *r = &PROFILES[SUBSONIC].r;
    uint16_t main_m = (uint16_t)(plant_apogee(r, &ISA) - 100.0f);
    const mp_charge_t charges[] = {{0.0f, 0.0f}, {1000.0f, 0.1f}, {5000.0f, 0.3f}};
    char bad[256] = "";
    float worst = 0.0f;
    for (unsigned k = 0; k < sizeof(charges) / sizeof(charges[0]); k++) {
        for (uint32_t seed = 1; seed <= 10; seed++) {
            conditions_t c;
            memset(&c, 0, sizeof(c));
            c.charge = charges[k];
            c.main_m = main_m;
            mach_result_t res = fly_mach(r, &ISA, &c, seed, 120.0f);
            float err = res.main ? res.main_h - (float)main_m : 1e6f;
            if (fabsf(err) > worst)
                worst = fabsf(err);
            if (!res.drogue || fabsf(err) > MAIN_TOL_M) {
                char item[64];
                snprintf(item, sizeof(item), " %.0f Pa/%u: %+.0f m;", (double)charges[k].peak_pa, (unsigned)seed,
                         (double)err);
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    printf("  main at %u m, through charges of up to 5 kPa: within %.1f m%s%s\n", (unsigned)main_m, (double)worst,
           bad[0] ? "; wrong:" : "", bad);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* Under a swinging canopy the pressure noise is several times the pad's, and
 * no fit is clean: the triggers act once their 2 s wait is up, and the
 * landing, once the swing stops at touchdown, is found as quickly as ever. */
void test_T5_canopy_swing(void) {
    const mp_rocket_t *r = &PROFILES[SUBSONIC].r;
    uint16_t main_m = (uint16_t)(plant_apogee(r, &ISA) - 100.0f);
    char bad[256] = "";
    float worst_main = 0.0f, worst_land = 0.0f;
    for (uint32_t seed = 1; seed <= 10; seed++) {
        conditions_t c;
        memset(&c, 0, sizeof(c));
        c.swing_rms_pa = 5.0f * SENSOR_RMS_PA;
        c.main_m = main_m;
        c.main_ms = 6.0f;
        c.to_landed = true;
        mach_result_t res = fly_mach(r, &ISA, &c, seed, 400.0f);
        float err = res.main ? res.main_h - (float)main_m : 1e6f;
        float land = res.landed_t > 0.0f && res.touchdown_t > 0.0f ? res.landed_t - res.touchdown_t : 1e6f;
        worst_main = fabsf(err) > worst_main ? fabsf(err) : worst_main;
        worst_land = fabsf(land) > worst_land ? fabsf(land) : worst_land;
        if (fabsf(err) > MAIN_TOL_M || land < 0.0f || land > 3.0f) {
            char item[64];
            snprintf(item, sizeof(item), " %u: main %+.0f m, LANDED %+.1f s;", (unsigned)seed, (double)err,
                     (double)land);
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    printf("  canopy swing at %.0f Pa: main within %.1f m, LANDED within %.1f s of touchdown%s%s\n",
           (double)(5.0f * SENSOR_RMS_PA), (double)worst_main, (double)worst_land, bad[0] ? "; wrong:" : "", bad);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* ── M1: the Mach lockout ─────────────────────────────────────────── */

static const mp_site_t *const SITES[] = {&COLD, &HOT};

static float plant_apogee_t(const mp_rocket_t *r, const mp_site_t *s) {
    mp_state_t st;
    mp_launch(&st);
    while (!st.apogee && st.t < 200.0f)
        mp_step(&st, s, r, 0.001f);
    return st.apogee_t;
}

static void drogue_after_apogee(const mach_result_t *r, const char *what, char *bad, size_t cap) {
    if (r->drogue && r->drogue_t >= r->apogee_t && r->drogue_t - r->apogee_t <= 1.5f)
        return;
    char item[96];
    if (r->drogue)
        snprintf(item, sizeof(item), " %s: drogue %+.2f s from apogee;", what, (double)(r->drogue_t - r->apogee_t));
    else
        snprintf(item, sizeof(item), " %s: no drogue;", what);
    strncat(bad, item, cap - 1 - strlen(bad));
}

/* [FLT-MACH-02] A flight that never nears Mach 1 is never locked, and deploys
 * as a plain altimeter would. */
void test_M1_subsonic_never_locks(void) {
    char bad[256] = "";
    for (int si = 0; si < 2; si++) {
        conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        mach_result_t r = fly_mach(&PROFILES[SUBSONIC].r, SITES[si], &c, 3, 120.0f);
        if (r.locked)
            strncat(bad, si ? " locked at the hot pad;" : " locked at the cold pad;", sizeof(bad) - 1 - strlen(bad));
        drogue_after_apogee(&r, si ? "hot" : "cold", bad, sizeof(bad));
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-02] The flag is set while the data is still clean: before Mach
 * 0.85, where a port's error begins -- on a 30 g boost too, whose ignition
 * step spoils the fits for the first second. */
void test_M1_flag_before_mach_085(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    char bad[256] = "";
    float worst = 0.0f;
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            conditions_t c;
            memset(&c, 0, sizeof(c));
            mach_result_t r = fly_mach(&PROFILES[fast[i]].r, SITES[si], &c, 4, PROFILES[fast[i]].r.burn_s + 2.0f);
            worst = r.locked && r.flag_mach > worst ? r.flag_mach : worst;
            if (!r.locked || r.flag_mach >= 0.85f) {
                char item[64];
                snprintf(item, sizeof(item), " %s %s: %s Mach %.2f;", PROFILES[fast[i]].name, si ? "hot" : "cold",
                         r.locked ? "flagged at" : "never flagged, peak", (double)(r.locked ? r.flag_mach : r.max_mach));
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    printf("  flagged by Mach %.2f at the latest\n", (double)worst);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-03] A peak between the flag and Mach 0.85 is locked for nothing,
 * and let go soon after burnout: once the burnout's step has left the fit's
 * window (1 s) and the rocket has slowed below the release speed, a second of
 * the coast's signature. Within 3 s, and well before apogee. */
void test_M1_mid_mach_releases(void) {
    const mp_rocket_t *rk = &PROFILES[MID_MACH].r;
    char bad[256] = "";
    int flagged = 0;
    for (int si = 0; si < 2; si++) {
        conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        mach_result_t r = fly_mach(rk, SITES[si], &c, 5, 120.0f);
        drogue_after_apogee(&r, si ? "hot" : "cold", bad, sizeof(bad));
        if (!r.locked)
            continue;
        flagged++;
        printf("  mid-Mach %s: flagged at Mach %.2f, released %.2f s after burnout at Mach %.2f\n", si ? "hot" : "cold",
               (double)r.flag_mach, (double)(r.release_t - rk->burn_s), (double)r.release_mach);
        if (!r.released || r.release_t - rk->burn_s > 3.0f || r.apogee_t - r.release_t < 5.0f) {
            char item[64];
            snprintf(item, sizeof(item), " %s: released %s;", si ? "hot" : "cold", r.released ? "late" : "never");
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(flagged > 0, "the cold pad's Mach 0.76 passes the flag");
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-05] Supersonic, with the port's error either way or none, the
 * drogue never fires before the true apogee, and fires within 1.5 s of it. */
void test_M1_no_drogue_before_apogee(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_READS_HIGH, &PORT_READS_LOW, &PORT_FAKES_DESCENT};
    const char *names[] = {"clean", "high", "low", "fakes descent"};
    char bad[512] = "";
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 4; pi++) {
                conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi])
                    c.port = *ports[pi];
                c.stop_after_apogee = true;
                mach_result_t r = fly_mach(&PROFILES[fast[i]].r, SITES[si], &c, 6, 120.0f);
                char what[48];
                snprintf(what, sizeof(what), "%s %s %s", PROFILES[fast[i]].name, si ? "hot" : "cold", names[pi]);
                drogue_after_apogee(&r, what, bad, sizeof(bad));
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-03] At altitude the release margin is thinnest: the low-drag
 * flight to 10 km from the hot, high pad releases before apogee on every
 * seed. */
void test_M1_release_at_altitude(void) {
    int late = 0, never = 0;
    float least = 1e9f;
    for (uint32_t seed = 1; seed <= 1000; seed++) {
        conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        mach_result_t r = fly_mach(&PROFILES[LOW_DRAG].r, &HOT, &c, seed, 120.0f);
        if (!r.released) {
            never++;
            continue;
        }
        float margin = r.apogee_t - r.release_t;
        least = margin < least ? margin : least;
        late += margin <= 0.0f;
    }
    printf("  low-drag from the hot pad: released at least %.1f s before apogee over 1000 seeds; %d late, %d never\n",
           (double)least, late, never);
    TEST_ASSERT_EQUAL_INT(0, never);
    TEST_ASSERT_EQUAL_INT(0, late);
}

/* [FLT-MACH-04] Ports too noisy for any clean fit through the coast: the lock
 * can never release. Once clean fits show the rocket back below the pressure
 * the flag was set at, falling for a second, the drogue fires -- late, never
 * early. */
void test_M1_fallback(void) {
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.coast_noise_pa = 10.0f * SENSOR_RMS_PA;
    mach_result_t r = fly_mach(&PROFILES[MID_MACH].r, &COLD, &c, 8, 200.0f);
    char msg[160];
    snprintf(msg, sizeof(msg), "locked %d released %d fallback %d; back past the flag at %.2f s, drogue at %.2f s", r.locked,
             r.released, r.fallback, (double)r.return_t, (double)r.drogue_t);
    printf("  fallback: flagged %.0f m up; drogue %.2f s after the rocket fell back past it\n", (double)r.flag_h,
           (double)(r.drogue_t - r.return_t));
    TEST_ASSERT_TRUE_MESSAGE(r.locked && !r.released && r.fallback, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.return_t > 0.0f && r.drogue && r.drogue_t >= r.return_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t - r.return_t <= 1.5f, msg);
}

/* [FLT-MACH-07] The reported peak is the lowest pressure a clean fit showed
 * outside the lock: a port that reads low while supersonic reads the rocket
 * kilometres high, and none of it may reach the report. A lock that let go
 * within 2 s of apogee marks the peak a lower bound. On the standard
 * atmosphere's pad, where pressure altitude is the true height. */
void test_M1_peak_outside_lock(void) {
    const mp_port_t reads_very_low = {-1.0f, 0.06f, 0.15f, 0.09f};
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.port = reads_very_low;
    c.stop_after_apogee = true;
    mach_result_t r = fly_mach(&PROFILES[DRAGGY].r, &ISA, &c, 9, 120.0f);
    char msg[128];
    snprintf(msg, sizeof(msg), "reported %.0f m, true apogee %.0f m, lower bound %d", r.peak_cm / 100.0,
             (double)r.apogee_h, r.peak_lower_bound);
    printf("  draggy, a port reading 15%% of q low: %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.peak_cm / 100.0 - (double)r.apogee_h) <= 5.0, msg);
    TEST_ASSERT_FALSE_MESSAGE(r.peak_lower_bound, msg);

    /* Noisy ports until 2.5 s before apogee: the release comes inside the
     * last 2 s. */
    const mp_rocket_t *mm = &PROFILES[MID_MACH].r;
    memset(&c, 0, sizeof(c));
    c.coast_noise_pa = 10.0f * SENSOR_RMS_PA;
    c.coast_noise_to_s = plant_apogee_t(mm, &ISA) - 2.5f;
    c.stop_after_apogee = true;
    r = fly_mach(mm, &ISA, &c, 10, 120.0f);
    snprintf(msg, sizeof(msg), "released %.2f s before apogee, lower bound %d", (double)(r.apogee_t - r.release_t),
             r.peak_lower_bound);
    TEST_ASSERT_TRUE_MESSAGE(r.released && r.apogee_t - r.release_t < 2.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.peak_lower_bound, msg);
}

/* [FLT-MACH-02..06] Each integer comparison is its real threshold exactly,
 * from 1 to 120 kPa, whatever the fit says: rates and accelerations far past
 * any flight are clamped, not overflowed. */
void test_M1_integer_forms(void) {
    const int32_t rates[] = {-2000000000, -5000000, -100000, -4000, -3000, -2900, -2200, -1, 0, 1, 90, 4000, 2000000000};
    int wrong = 0, cases = 0;
    for (int32_t p = 1000; p <= 120000; p += 997) {
        for (unsigned k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
            /* The thresholds themselves, and a pascal either side. */
            int32_t near[] = {rates[k], -(29 * p) / 1000, -(29 * p) / 1000 - 1, -(22 * p) / 1000, -(22 * p) / 1000 + 1,
                              (9 * p) / 10000, (9 * p) / 10000 - 1};
            for (unsigned j = 0; j < sizeof(near) / sizeof(near[0]); j++) {
                int64_t r = near[j];
                cases++;
                wrong += mach_too_fast(p, (float)r) != (-r * 1000 > (int64_t)29 * p);
                wrong += mach_slow_ascent(p, (float)r) != (r < 0 && -r * 1000 < (int64_t)22 * p);
                wrong += mach_decelerating(p, (float)r) != (r * 10000 >= (int64_t)9 * p);
            }
        }
        cases++;
        wrong += mach_above_arm_height(p, 101325) != ((int64_t)p * 10000 < (int64_t)9965 * 101325);
        wrong += mach_above_arm_height(p, p + 1) != ((int64_t)p * 10000 < (int64_t)9965 * (p + 1));
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%d of %d comparisons differ from the threshold", wrong, cases);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, wrong, msg);
}

/* docs/mach_lockout.md's tables are this program's output: each threshold
 * over the envelope's air, 216 K at the tropopause to a 45 degree pad, and the
 * fit's noise. */
static double mach_per_rate(double t_k) {
    return MP_G * sqrt(MP_GAMMA / (MP_R * t_k)); /* -pdot/p of Mach 1 */
}

static double g_per_pddot(double t_k) {
    return MP_G * MP_G / (MP_R * t_k); /* pddot/p of 1 g */
}

static int doc_has(const char *doc, const char *row, char *missing, size_t cap) {
    if (strstr(doc, row))
        return 0;
    strncat(missing, "\n    ", cap - 1 - strlen(missing));
    strncat(missing, row, cap - 1 - strlen(missing));
    return 1;
}

void test_M1_design_note(void) {
    static char doc[65536];
    FILE *f = fopen("docs/mach_lockout.md", "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "docs/mach_lockout.md");
    size_t n = fread(doc, 1, sizeof(doc) - 1, f);
    fclose(f);
    doc[n] = '\0';
    const double cold = 216.65, hot = 318.15, pad_lo = 283.15;
    char row[160], missing[2048] = "";
    int absent = 0;
    snprintf(row, sizeof(row), "| Set flag | `1000*(-pdot) > 29*p` | Mach %.2f to %.2f |", 0.029 / mach_per_rate(cold),
             0.029 / mach_per_rate(hot));
    absent += doc_has(doc, row, missing, sizeof(missing));
    snprintf(row, sizeof(row), "| Release: slow | `pdot < 0 && 1000*(-pdot) < 22*p` | Mach %.2f to %.2f |",
             0.022 / mach_per_rate(cold), 0.022 / mach_per_rate(hot));
    absent += doc_has(doc, row, missing, sizeof(missing));
    snprintf(row, sizeof(row), "| Release: decelerating | `10000*pddot >= 9*p` | %.2f g to %.2f g |",
             0.0009 / g_per_pddot(cold), 0.0009 / g_per_pddot(hot));
    absent += doc_has(doc, row, missing, sizeof(missing));
    snprintf(row, sizeof(row), "| Minimum-altitude arm | `10000*p < 9965*p0` | %.1f m to %.1f m |",
             -log(0.9965) * MP_R * pad_lo / MP_G, -log(0.9965) * MP_R * hot / MP_G);
    absent += doc_has(doc, row, missing, sizeof(missing));
    snprintf(row, sizeof(row), "| Apogee | `p >= 1.0001*p_min` | %.2f m to %.2f m |", -log(1.0 / 1.0001) * MP_R * cold / MP_G,
             -log(1.0 / 1.0001) * MP_R * hot / MP_G);
    absent += doc_has(doc, row, missing, sizeof(missing));

    /* The fit's noise, sigma times the root of the sum of its coefficients'
     * squares: one second of samples at 50 Hz, evaluated at the newest. */
    double s2 = 0.0, s3 = 0.0, s4 = 0.0, tm = 0.0;
    const int N = 51;
    for (int i = 0; i < N; i++)
        tm += -1.0 + i / 50.0;
    tm /= N;
    for (int i = 0; i < N; i++) {
        double u = -1.0 + i / 50.0 - tm;
        s2 += u * u;
        s3 += u * u * u;
        s4 += u * u * u * u;
    }
    double det = N * (s2 * s4 - s3 * s3) - s2 * s2 * s2, u0 = -tm, c1 = 0.0, c2 = 0.0;
    for (int i = 0; i < N; i++) {
        double u = -1.0 + i / 50.0 - tm;
        double b = (N * (u * s4 - s3 * u * u) + s2 * (s3 - s2 * u)) / det;
        double c = (N * (s2 * u * u - s3 * u) - s2 * s2) / det;
        c1 += (b + 2.0 * c * u0) * (b + 2.0 * c * u0);
        c2 += (2.0 * c) * (2.0 * c);
    }
    const double sigma = SENSOR_RMS_PA;
    double rho_g_sea = 101325.0 / (MP_R * 288.15) * MP_G, rho_g_9k = 30742.0 / (MP_R * 229.65) * MP_G;
    snprintf(row, sizeof(row), "| pdot | %.2f Pa/s | %.2f m/s | %.2f m/s |", sigma * sqrt(c1), sigma * sqrt(c1) / rho_g_sea,
             sigma * sqrt(c1) / rho_g_9k);
    absent += doc_has(doc, row, missing, sizeof(missing));
    snprintf(row, sizeof(row), "| pddot | %.2f Pa/s^2 | %.3f g | %.3f g |", sigma * sqrt(c2),
             sigma * sqrt(c2) / rho_g_sea / MP_G, sigma * sqrt(c2) / rho_g_9k / MP_G);
    absent += doc_has(doc, row, missing, sizeof(missing));
    if (absent)
        printf("  rows docs/mach_lockout.md lacks:%s\n", missing);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, absent, "docs/mach_lockout.md's tables must be this program's output");
}

/* What a port's error must do to fake the release: the fakes-descent shape
 * scaled, both signs, on the fast profiles. Reported for
 * docs/mach_lockout.md's risks. The lock must never let go before burnout or
 * while the port's error is live, and the drogue must never come early. */
void test_M1_port_error_margin(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const float scales[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    char bad[512] = "";
    float worst = 0.0f;
    printf("  release Mach, by port error (x fakes-descent), cold pad:\n");
    for (unsigned i = 0; i < 3; i++) {
        char line[200];
        snprintf(line, sizeof(line), "    %-9s", PROFILES[fast[i]].name);
        for (int sign = -1; sign <= 1; sign += 2) {
            for (unsigned k = 0; k < sizeof(scales) / sizeof(scales[0]); k++) {
                conditions_t c;
                memset(&c, 0, sizeof(c));
                c.port = PORT_FAKES_DESCENT;
                c.port.sign = (float)sign;
                c.port.below *= scales[k];
                c.port.above *= scales[k];
                c.port.slope *= scales[k];
                c.stop_after_apogee = true;
                mach_result_t r = fly_mach(&PROFILES[fast[i]].r, &COLD, &c, 11, 120.0f);
                float m = fabsf(r.release_mach);
                worst = r.released && m > worst ? m : worst;
                size_t l = strlen(line);
                snprintf(line + l, sizeof(line) - l, " %+.2g:%.2f", (double)(sign * scales[k]), (double)m);
                char what[48];
                snprintf(what, sizeof(what), "%s x%+.2g", PROFILES[fast[i]].name, (double)(sign * scales[k]));
                if (r.released && (r.release_t < PROFILES[fast[i]].r.burn_s || m >= 0.85f)) {
                    strncat(bad, " ", sizeof(bad) - 1 - strlen(bad));
                    strncat(bad, what, sizeof(bad) - 1 - strlen(bad));
                    strncat(bad, ": released in the error;", sizeof(bad) - 1 - strlen(bad));
                }
                drogue_after_apogee(&r, what, bad, sizeof(bad));
            }
        }
        printf("%s\n", line);
    }
    printf("  released by Mach %.2f at the latest\n", (double)worst);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* ── M2: sensor failure in flight ─────────────────────────────────── */

static bool log_has(const char *event) {
    static char log[65536];
    int n = hal_fs_read_file("flight_log.csv", log, (int)sizeof(log) - 1);
    if (n <= 0)
        return false;
    log[n] = '\0';
    char field[40];
    snprintf(field, sizeof(field), ",%s", event);
    return strstr(log, field) != NULL;
}

/* [SNS-PRES-10] A sensor that sticks in coast, and stays stuck, deploys
 * nothing: armed on the subsonic flight, locked on the mid-Mach one. */
void test_M2_stuck_in_coast(void) {
    const struct {
        unsigned prof;
        bool locked;
    } cases[] = {{SUBSONIC, false}, {MID_MACH, true}};
    char bad[256] = "";
    for (unsigned i = 0; i < 2; i++) {
        const mp_rocket_t *rk = &PROFILES[cases[i].prof].r;
        conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stuck_at_s = cases[i].locked ? rk->burn_s + 0.5f : plant_apogee_t(rk, &COLD) - 0.5f;
        mach_result_t r = fly_mach(rk, &COLD, &c, 12, 120.0f);
        bool state_ok = cases[i].locked ? ctx.mach_lock || !r.released : ctx.pyros_armed;
        if (mock_pyro.fire_count != 0 || !state_ok) {
            char item[96];
            snprintf(item, sizeof(item), " %s: %d fires, %s;", PROFILES[cases[i].prof].name, mock_pyro.fire_count,
                     state_ok ? "as it should be" : (cases[i].locked ? "not locked" : "not armed"));
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [SNS-PRES-11] Half a second without samples, across the apogee: no fit is
 * clean until a whole window of new samples exists, so the apogee waits for
 * it. */
void test_M2_dropout_in_coast(void) {
    const mp_rocket_t *rk = &PROFILES[SUBSONIC].r;
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.dropout_at_s = plant_apogee_t(rk, &COLD) - 0.3f;
    c.dropout_s = 0.5f;
    c.stop_after_apogee = true;
    mach_result_t r = fly_mach(rk, &COLD, &c, 13, 120.0f);
    float back = c.dropout_at_s + c.dropout_s;
    char msg[128];
    snprintf(msg, sizeof(msg), "samples back at %.2f s, drogue at %.2f s, apogee at %.2f s", (double)back,
             (double)r.drogue_t, (double)r.apogee_t);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t >= back + 1.0f, msg);
}

/* [SNS-PRES-11] Two seconds without samples under the drogue, the main set
 * where the rocket falls meanwhile: the loss is flagged, and the main waits
 * for a whole window of new samples rather than fire on the old ones. */
void test_M2_lost(void) {
    const mp_rocket_t *rk = &PROFILES[SUBSONIC].r;
    float ap_t = plant_apogee_t(rk, &ISA);
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.main_m = (uint16_t)(plant_apogee(rk, &ISA) - 100.0f);
    c.dropout_at_s = ap_t + 4.5f;
    c.dropout_s = 2.0f;
    mach_result_t r = fly_mach(rk, &ISA, &c, 14, 120.0f);
    float back = c.dropout_at_s + c.dropout_s;
    char msg[160];
    snprintf(msg, sizeof(msg), "lost %.2f-%.2f s; main at %.2f s, %.0f m (set %u m); diag 0x%x", (double)c.dropout_at_s,
             (double)back, (double)r.main_t, (double)r.main_h, (unsigned)c.main_m, (unsigned)ctx.diag);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_SENSOR_LOST) != 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(log_has("SENSOR_LOST"), msg);
    TEST_ASSERT_TRUE_MESSAGE(r.main && r.main_t >= back + 1.0f, msg);
}

/* [SNS-PRES-06] guard: readings no atmosphere can produce, half a second of
 * them in coast, are discarded and counted, and deploy nothing early. */
void test_M2_out_of_range(void) {
    const mp_rocket_t *rk = &PROFILES[SUBSONIC].r;
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.glitch_at_s = plant_apogee_t(rk, &COLD) - 1.0f;
    c.glitch_pa = 150000;
    c.glitch_n = 25;
    c.stop_after_apogee = true;
    mach_result_t r = fly_mach(rk, &COLD, &c, 15, 120.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "%u rejected; drogue %+.2f s from apogee", (unsigned)mock_pres_rejects,
             (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(mock_pres_rejects >= 25, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
}

/* [SNS-PRES-10, SNS-PRES-11] Each failure is a DIAG bit, which /api/status
 * lists by name, and an event in the log. A sensor that comes back flies on:
 * stuck for 2 s, or gone for 1, a second or more before apogee. */
void test_M2_reported(void) {
    const mp_rocket_t *rk = &PROFILES[SUBSONIC].r;
    float ap_t = plant_apogee_t(rk, &COLD);
    conditions_t c;
    memset(&c, 0, sizeof(c));
    c.stuck_at_s = ap_t - 4.0f;
    c.stuck_s = 2.0f;
    c.stop_after_apogee = true;
    mach_result_t r = fly_mach(rk, &COLD, &c, 16, 120.0f);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_SENSOR_STUCK) != 0, "a stuck sensor sets its DIAG bit");
    TEST_ASSERT_EQUAL_STRING("sensor_stuck", flight_diag_name(DIAG_SENSOR_STUCK));
    TEST_ASSERT_TRUE_MESSAGE(log_has("SENSOR_STUCK"), "and logs it");
    char msg[96];
    snprintf(msg, sizeof(msg), "stuck: drogue %d, %+.2f s from apogee", r.drogue, (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 1.5f, msg);

    memset(&c, 0, sizeof(c));
    c.dropout_at_s = ap_t - 4.0f;
    c.dropout_s = 1.0f;
    c.stop_after_apogee = true;
    r = fly_mach(rk, &COLD, &c, 17, 120.0f);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_SENSOR_LOST) != 0, "a lost sensor sets its DIAG bit");
    TEST_ASSERT_EQUAL_STRING("sensor_lost", flight_diag_name(DIAG_SENSOR_LOST));
    TEST_ASSERT_TRUE_MESSAGE(log_has("SENSOR_LOST"), "and logs it");
    snprintf(msg, sizeof(msg), "lost: drogue %d, %+.2f s from apogee", r.drogue, (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 1.5f, msg);
}

/* [SNS-PRES-10] guard: a sensor with the MS5607's noise never reads as stuck,
 * over an hour on the pad. */
void test_M2_real_sensor_never_stuck(void) {
    boot_like_hardware(18);
    uint32_t t = 0;
    run_to_pad(&t);
    for (uint32_t end = t + 3600000u; t < end; t++)
        tick(t);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, ctx.diag & DIAG_SENSOR_STUCK, "an hour of real noise read as a stuck sensor");
}

/* ── T9: the lockout at the MS5607's ~90 Hz ───────────────────────── */

/* The fast profiles, both pads, clean ports and the one that fakes a
 * descent: flagged before Mach 0.85, released before apogee, and the drogue
 * after it, sampled every 11 ms as at 20. */
void test_T9_mach_at_90hz(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_FAKES_DESCENT};
    char bad[512] = "";
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 2; pi++) {
                conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi])
                    c.port = *ports[pi];
                c.stop_after_apogee = true;
                c.interval_ms = 11u;
                mach_result_t r = fly_mach(&PROFILES[fast[i]].r, SITES[si], &c, 20, 120.0f);
                char what[48];
                snprintf(what, sizeof(what), "%s %s%s", PROFILES[fast[i]].name, si ? "hot" : "cold",
                         pi ? " fakes descent" : "");
                drogue_after_apogee(&r, what, bad, sizeof(bad));
                if (!r.locked || r.flag_mach >= 0.85f || !r.released || r.release_t >= r.apogee_t) {
                    char item[96];
                    snprintf(item, sizeof(item), " %s: flag Mach %.2f, released %d;", what, (double)r.flag_mach,
                             r.released);
                    strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_M0_atmosphere);
    RUN_TEST(test_M0_mach);
    RUN_TEST(test_M0_port_error);
    RUN_TEST(test_M0_charge);
    RUN_TEST(test_M0_failures);
    RUN_TEST(test_M0_profiles);
    RUN_TEST(test_M0_report);
    RUN_TEST(test_T5_ejection);
    RUN_TEST(test_T5_canopy_swing);
    RUN_TEST(test_M1_subsonic_never_locks);
    RUN_TEST(test_M1_flag_before_mach_085);
    RUN_TEST(test_M1_mid_mach_releases);
    RUN_TEST(test_M1_no_drogue_before_apogee);
    RUN_TEST(test_M1_release_at_altitude);
    RUN_TEST(test_M1_fallback);
    RUN_TEST(test_M1_peak_outside_lock);
    RUN_TEST(test_M1_integer_forms);
    RUN_TEST(test_M1_design_note);
    RUN_TEST(test_M1_port_error_margin);
    RUN_TEST(test_M2_stuck_in_coast);
    RUN_TEST(test_M2_dropout_in_coast);
    RUN_TEST(test_M2_lost);
    RUN_TEST(test_M2_out_of_range);
    RUN_TEST(test_M2_reported);
    RUN_TEST(test_M2_real_sensor_never_stuck);
    RUN_TEST(test_T9_mach_at_90hz);
    return UNITY_END();
}
