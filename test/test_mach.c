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

void setUp(void) {}
void tearDown(void) {}

/* ── The sites and the rockets ────────────────────────────────────── */

static const mp_site_t COLD = {10.0f, 0.0f};   /* 10 °C at sea level */
static const mp_site_t HOT = {45.0f, 2000.0f}; /* 45 °C at 2000 m */

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
    float swing_rms_pa;
} conditions_t;

typedef struct {
    bool launched;
    bool drogue;
    float drogue_t, apogee_t, apogee_h, max_mach;
    bool gate_latched, released;
    float release_t, release_mach;
} mach_result_t;

static mach_result_t fly_mach(const mp_rocket_t *r, const mp_site_t *s, const conditions_t *c, uint32_t seed,
                              float until_s) {
    mach_result_t res;
    memset(&res, 0, sizeof(res));
    boot_like_hardware(seed);
    float pad_pa = mp_pad_pa(s);
    mock_pressure.pressure_pa = pad_pa;
    uint32_t t = 0;
    uint32_t pad = run_to_pad(&t);
    uint32_t ign = pad + 10000u;
    mp_state_t st;
    mp_launch(&st);
    int fires = 0;
    float charge_at = -1.0f;
    for (; t < ign + (uint32_t)(until_s * 1000.0f); t++) {
        float tf = ((float)t - (float)ign) / 1000.0f;
        if (tf >= 0.0f)
            mp_step(&st, s, r, 0.001f);
        float static_pa = mp_pressure_pa(s, st.h);
        float sensed = static_pa + mp_port_error_pa(&c->port, st.mach, static_pa);
        if (charge_at >= 0.0f)
            sensed += mp_charge_pa(&c->charge, tf - charge_at);
        mock_pressure.pressure_pa = sensed;
        bool out = c->dropout_s > 0.0f && tf >= c->dropout_at_s && tf < c->dropout_at_s + c->dropout_s;
        mock_pressure.sensor_type = out ? 0 : 2;
        mock_sensor_stuck = c->stuck_at_s > 0.0f && tf >= c->stuck_at_s;
        if (st.canopy && c->swing_rms_pa > 0.0f)
            mock_noise_rms_pa = c->swing_rms_pa;
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
            }
        }
        res.gate_latched |= ctx.mach_exceeded;
        if (!res.released && ctx.mach_exceeded && ctx.pyros_armed && ctx.subsonic_since != 0 &&
            t - ctx.subsonic_since >= 1000u) {
            res.released = true;
            res.release_t = tf;
            res.release_mach = st.mach;
        }
        if (st.landed)
            break;
    }
    res.apogee_t = st.apogee_t;
    res.apogee_h = st.apogee_h;
    res.max_mach = st.max_mach;
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
    printf("  %-9s %-5s %-13s %-9s %-17s %s\n", "profile", "pad", "ports", "max Mach", "drogue", "gate release");
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
                if (!r.gate_latched)
                    snprintf(release, sizeof(release), "never latched");
                else if (r.released)
                    snprintf(release, sizeof(release), "Mach %.2f, %.1f s before apogee", r.release_mach,
                             r.apogee_t - r.release_t);
                else
                    snprintf(release, sizeof(release), "never released");
                printf("  %-9s %2.0f C  %-13s %-9.2f %-17s %s\n", PROFILES[i].name, sites[si]->temp_c, ports[pi].name,
                       r.max_mach, drogue, release);
            }
        }
    }
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
    return UNITY_END();
}
