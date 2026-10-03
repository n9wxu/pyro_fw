/*
 * Flights with a truth beside them [TST-03]: supersonic and subsonic rockets
 * from a cold sea-level pad and a hot high one, with static ports that
 * misreport while the rocket is fast, ejection charges that pressurise the
 * bay, and sensors that stop or stick. The firmware flies each one through
 * the board harness, and the plant says what really happened.
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
#include "../src/pressure_processing.h"
#include "board_harness.h"
#include "flight_run.h"
#include "mach_plant.h"
#include "../src/estimator.h"
#include "../src/pad_check.h"

void setUp(void) {}
void tearDown(void) {}

/* ── The ports ────────────────────────────────────────────────────── */

/* A persistent error for the whole supersonic period, stepping at Mach 1. */
static const mp_port_t PORT_READS_HIGH = {+1.0f, 0.02f, 0.05f, 0.03f};
static const mp_port_t PORT_READS_LOW = {-1.0f, 0.02f, 0.05f, 0.03f};
/* One that grows fast enough through the boost to make a climbing rocket
 * look as if it were falling. */
static const mp_port_t PORT_FAKES_DESCENT = {+1.0f, 0.06f, 0.14f, 0.05f};

/* ── M0: the plant itself ─────────────────────────────────────────── */

/* The closed form against the hydrostatic equation integrated a metre at a
 * time, through the tropopause. */
void test_TST_03_plant_atmosphere(void) {
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

void test_TST_03_plant_mach(void) {
    mp_state_t st;
    mp_launch(&st);
    for (int i = 0; i < 3000; i++) {
        mp_step(&st, &HOT, &ROCKETS[DRAGGY].r, 0.001f);
        if (i % 500 == 499) {
            float want = st.v / sqrtf(MP_GAMMA * MP_R * mp_temp_k(&HOT, st.h));
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, want, st.mach);
        }
    }
}

void test_TST_03_plant_port_error(void) {
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
    while (st.t < ROCKETS[DRAGGY].r.burn_s) {
        mp_step(&st, &COLD, &ROCKETS[DRAGGY].r, 0.001f);
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

void test_TST_03_plant_charge(void) {
    const mp_charge_t c = {3000.0f, 0.2f};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mp_charge_pa(&c, -0.1f));
    TEST_ASSERT_EQUAL_FLOAT(3000.0f, mp_charge_pa(&c, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 3000.0f / 2.71828f, mp_charge_pa(&c, 0.2f));
}

void test_TST_03_plant_sensor_failures(void) {
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
        mp_step(&st, s, &ROCKETS[i].r, 0.001f);
        if (fabsf(st.mach) >= 1.0f) {
            if (sup_from < 0.0f)
                sup_from = st.t;
            sup_to = st.t;
        }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%s from the %.0f C pad: Mach %.2f, apogee %.0f m", ROCKETS[i].name, s->temp_c,
             st.max_mach, st.apogee_h);
    TEST_ASSERT_TRUE_MESSAGE(st.max_mach >= lo && st.max_mach <= hi, msg);
    if (i == LOW_DRAG) {
        snprintf(msg, sizeof(msg), "low-drag supersonic for %.1f s, apogee %.0f m", sup_to - sup_from, st.apogee_h);
        TEST_ASSERT_TRUE_MESSAGE(sup_to - sup_from >= 10.0f && st.apogee_h > 9000.0f && st.apogee_h < 11000.0f, msg);
    }
}

void test_TST_03_plant_profiles(void) {
    profile_reaches(SUBSONIC, &COLD, 0.0f, 0.6f);
    profile_reaches(SUBSONIC, &HOT, 0.0f, 0.6f);
    profile_reaches(MID_MACH, &COLD, 0.65f, 0.85f);
    profile_reaches(MID_MACH, &HOT, 0.65f, 0.85f);
    profile_reaches(DRAGGY, &COLD, 1.45f, 1.55f);
    profile_reaches(DRAGGY, &HOT, 1.45f, 2.1f);
    profile_reaches(LOW_DRAG, &COLD, 1.5f, 1.7f);
    profile_reaches(LOW_DRAG, &HOT, 1.4f, 1.7f);
    profile_reaches(BOOST_30G, &COLD, 0.86f, 1.2f);
    const mp_rocket_t *b = &ROCKETS[BOOST_30G].r;
    float net_g = (b->thrust_n / (b->dry_kg + b->prop_kg) - MP_G) / MP_G;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 30.0f, net_g);
}

/* ── M0: the report ───────────────────────────────────────────────── */

/* For each profile, pad and port: when each estimator's drogue came against
 * the true apogee, flown obeying it. */
void test_TST_03_report(void) {
    const mp_site_t *sites[] = {&COLD, &HOT};
    const struct {
        const char *name;
        const mp_port_t *port;
    } ports[] = {{"clean", NULL},
                 {"reads high", &PORT_READS_HIGH},
                 {"reads low", &PORT_READS_LOW},
                 {"fakes descent", &PORT_FAKES_DESCENT}};
    printf("  %-9s %-5s %-13s %-9s drogue from apogee, s:", "profile", "pad", "ports", "max Mach");
    for (uint8_t e = 0; e < estimator_count(); e++)
        printf(" %-9s", estimator_at(e)->name);
    printf("\n");
    for (unsigned i = 0; i < N_ROCKETS; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 4; pi++) {
                if (pi > 0 && i < DRAGGY && i != BOOST_30G)
                    continue; /* subsonic flights see no port error */
                char line[160] = "";
                float max_mach = 0.0f;
                for (uint8_t e = 0; e < estimator_count(); e++) {
                    flight_conditions_t c;
                    memset(&c, 0, sizeof(c));
                    c.estimator = estimator_at(e)->name;
                    c.stop_after_apogee = true;
                    if (ports[pi].port)
                        c.port = *ports[pi].port;
                    flown_t r = fly(&ROCKETS[i].r, sites[si], &c, 7, 400.0f);
                    TEST_ASSERT_TRUE_MESSAGE(r.launched, ROCKETS[i].name);
                    max_mach = r.max_mach;
                    size_t l = strlen(line);
                    if (r.drogue)
                        snprintf(line + l, sizeof(line) - l, " %+9.2f", (double)(r.drogue_t - r.apogee_t));
                    else
                        snprintf(line + l, sizeof(line) - l, " %9s", "never");
                }
                printf("  %-9s %2.0f C  %-13s %-9.2f                       %s\n", ROCKETS[i].name,
                       (double)sites[si]->temp_c, ports[pi].name, (double)max_mach, line);
            }
        }
    }
}

/* ── T5: the fit through a charge and a swinging canopy ───────────── */

#define MAIN_TOL_M 8.0f

/* The drogue's charge pressurises the bay at apogee: to the pressure layer the
 * rocket has dropped hundreds of metres in an instant. Pressure triggers wait
 * for a clean fit, up to 2 s, so the main 100 m below still fires where it is
 * set. */
void test_PYR_MODE_06_a_charge_in_the_bay_does_not_move_the_main(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    uint16_t main_m = (uint16_t)(plant_apogee_m(r, &ISA) - 100.0f);
    const mp_charge_t charges[] = {{0.0f, 0.0f}, {1000.0f, 0.1f}, {5000.0f, 0.3f}};
    char bad[256] = "";
    float worst = 0.0f;
    for (unsigned k = 0; k < sizeof(charges) / sizeof(charges[0]); k++) {
        for (uint32_t seed = 1; seed <= 10; seed++) {
            flight_conditions_t c;
            memset(&c, 0, sizeof(c));
            c.charge = charges[k];
            c.main_m = main_m;
            flown_t res = fly(r, &ISA, &c, seed, 120.0f);
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
void test_SNS_EST_03_a_swinging_canopy_moves_neither_the_main_nor_the_landing(void) {
    const mp_rocket_t *r = &ROCKETS[SUBSONIC].r;
    uint16_t main_m = (uint16_t)(plant_apogee_m(r, &ISA) - 100.0f);
    char bad[256] = "";
    float worst_main = 0.0f, worst_land = 0.0f;
    for (uint32_t seed = 1; seed <= 10; seed++) {
        flight_conditions_t c;
        memset(&c, 0, sizeof(c));
        c.swing_rms_pa = 5.0f * SENSOR_RMS_PA;
        c.main_m = main_m;
        c.main_ms = 6.0f;
        c.to_landed = true;
        flown_t res = fly(r, &ISA, &c, seed, 400.0f);
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

/* ── M1: apogee near Mach 1, on every estimator ───────────────────── */

static const mp_site_t *const SITES[] = {&COLD, &HOT};

static void note(char *bad, size_t cap, const char *item) {
    strncat(bad, item, cap - 1 - strlen(bad));
}

static void drogue_after_apogee(const flown_t *r, const char *what, float within_s, char *bad, size_t cap) {
    if (r->drogue && r->drogue_t >= r->apogee_t && r->drogue_t - r->apogee_t <= within_s)
        return;
    char item[112];
    if (r->drogue)
        snprintf(item, sizeof(item), " %s: drogue %+.2f s from apogee;", what, (double)(r->drogue_t - r->apogee_t));
    else
        snprintf(item, sizeof(item), " %s: no drogue;", what);
    note(bad, cap, item);
}

static flight_conditions_t obeying(uint8_t estimator) {
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.estimator = estimator_at(estimator)->name;
    c.stop_after_apogee = true;
    return c;
}

/* [FLT-APO-07] A flight that never nears Mach 1 deploys as a plain altimeter
 * would. */
void test_FLT_APO_07_a_subsonic_flight_deploys_at_apogee(void) {
    const unsigned slow[] = {SUBSONIC, MID_MACH};
    char bad[512] = "";
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 2; i++) {
            for (int si = 0; si < 2; si++) {
                flight_conditions_t c = obeying(e);
                flown_t r = fly(&ROCKETS[slow[i]].r, SITES[si], &c, 3, 120.0f);
                char what[64];
                snprintf(what, sizeof(what), "%s %s %s", c.estimator, ROCKETS[slow[i]].name, si ? "hot" : "cold");
                drogue_after_apogee(&r, what, 1.5f, bad, sizeof(bad));
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-ASC-08] Nothing arms below about 30 m, whatever the boost. */
void test_FLT_ASC_08_nothing_arms_below_30_m(void) {
    const unsigned profiles[] = {HOP, SUBSONIC, BOOST_30G};
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 3; i++) {
            flight_conditions_t c = obeying(e);
            flown_t r = fly(&ROCKETS[profiles[i]].r, &COLD, &c, 25, 120.0f);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s %s: armed %d at %.1f m", c.estimator, ROCKETS[profiles[i]].name, r.armed,
                     (double)r.armed_h);
            TEST_ASSERT_TRUE_MESSAGE(r.armed && r.armed_h >= 28.0f, msg);
        }
    }
}

/* [FLT-APO-07] Supersonic, with the port's error either way or none, the
 * drogue never fires before the true apogee, and fires within 1.5 s of it. */
void test_FLT_APO_07_no_fire_before_apogee_when_supersonic(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_READS_HIGH, &PORT_READS_LOW, &PORT_FAKES_DESCENT};
    const char *names[] = {"clean", "high", "low", "fakes descent"};
    char bad[1024] = "";
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 3; i++) {
            for (int si = 0; si < 2; si++) {
                for (unsigned pi = 0; pi < 4; pi++) {
                    flight_conditions_t c = obeying(e);
                    if (ports[pi])
                        c.port = *ports[pi];
                    flown_t r = fly(&ROCKETS[fast[i]].r, SITES[si], &c, 6, 120.0f);
                    char what[64];
                    snprintf(what, sizeof(what), "%s %s %s %s", c.estimator, ROCKETS[fast[i]].name, si ? "hot" : "cold",
                             names[pi]);
                    drogue_after_apogee(&r, what, 1.5f, bad, sizeof(bad));
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-APO-07] The same port error scaled, both signs, on the fast profiles. */
void test_FLT_APO_07_no_fire_before_apogee_at_any_size_of_port_error(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const float scales[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    char bad[1024] = "";
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 3; i++) {
            for (int sign = -1; sign <= 1; sign += 2) {
                for (unsigned k = 0; k < sizeof(scales) / sizeof(scales[0]); k++) {
                    flight_conditions_t c = obeying(e);
                    c.port = PORT_FAKES_DESCENT;
                    c.port.sign = (float)sign;
                    c.port.below *= scales[k];
                    c.port.above *= scales[k];
                    c.port.slope *= scales[k];
                    flown_t r = fly(&ROCKETS[fast[i]].r, &COLD, &c, 11, 120.0f);
                    char what[64];
                    snprintf(what, sizeof(what), "%s %s x%+.2g", c.estimator, ROCKETS[fast[i]].name,
                             (double)(sign * scales[k]));
                    drogue_after_apogee(&r, what, 1.5f, bad, sizeof(bad));
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-APO-07] The low-drag flight to 10 km from the hot, high pad, on every
 * seed, obeying the default estimator. */
void test_FLT_APO_07_at_10_km_on_every_seed(void) {
    int early = 0, never = 0;
    float latest = 0.0f;
    for (uint32_t seed = 1; seed <= 1000; seed++) {
        flight_conditions_t c = obeying(0);
        flown_t r = fly(&ROCKETS[LOW_DRAG].r, &HOT, &c, seed, 120.0f);
        if (!r.drogue) {
            never++;
            continue;
        }
        float late = r.drogue_t - r.apogee_t;
        early += late < 0.0f;
        latest = late > latest ? late : latest;
    }
    printf("  low-drag from the hot pad, 1000 seeds: %d early, %d never, at most %.2f s after apogee\n", early, never,
           (double)latest);
    TEST_ASSERT_EQUAL_INT(0, early);
    TEST_ASSERT_EQUAL_INT(0, never);
    TEST_ASSERT_TRUE(latest <= 1.5f);
}

/* [FLT-APO-07, FLT-APO-08] No readings from just after burnout until half a
 * second before apogee: the climb is not seen, so the fall must last, and the
 * peak is marked a lower bound. The default estimator finds it within 5 s. */
void test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last(void) {
    const mp_rocket_t *rk = &ROCKETS[MID_MACH].r;
    for (uint8_t e = 0; e < estimator_count(); e++) {
        flight_conditions_t c = obeying(e);
        c.stop_after_apogee = false;
        c.dropout_at_s = rk->burn_s + 0.5f;
        c.dropout_s = plant_apogee_s(rk, &COLD) - 0.5f - c.dropout_at_s;
        flown_t r = fly(rk, &COLD, &c, 8, 200.0f);
        char msg[200];
        snprintf(msg, sizeof(msg), "%s: apogee %.2f s, drogue %d at %.2f s, lower bound %d", c.estimator,
                 (double)r.apogee_t, r.drogue, (double)r.drogue_t, r.peak_lower_bound);
        printf("  no readings through the coast, %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
        TEST_ASSERT_TRUE_MESSAGE(r.drogue_t - r.apogee_t >= 2.0f, msg);
        TEST_ASSERT_TRUE_MESSAGE(r.drogue_t - r.apogee_t <= (e == 0 ? 5.0f : 10.0f), msg);
        TEST_ASSERT_TRUE_MESSAGE(r.peak_lower_bound, msg);
    }
}

/* [FLT-APO-08] The reported peak is the lowest filtered pressure the
 * estimator explained: a port that reads low while supersonic reads the
 * rocket kilometres high, and none of it may reach the report. On the
 * standard atmosphere's pad, where pressure altitude is the true height. */
void test_FLT_APO_08_the_peak_is_from_what_the_estimator_explained(void) {
    const mp_port_t reads_very_low = {-1.0f, 0.06f, 0.15f, 0.09f};
    for (uint8_t e = 0; e < estimator_count(); e++) {
        flight_conditions_t c = obeying(e);
        c.port = reads_very_low;
        flown_t r = fly(&ROCKETS[DRAGGY].r, &ISA, &c, 9, 120.0f);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: reported %.0f m, true apogee %.0f m, lower bound %d", c.estimator,
                 r.peak_cm / 100.0, (double)r.apogee_h, r.peak_lower_bound);
        printf("  draggy, a port reading 15%% of q low, %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(r.peak_cm / 100.0 - (double)r.apogee_h) <= 5.0, msg);
        TEST_ASSERT_FALSE_MESSAGE(r.peak_lower_bound, msg);
    }
}

/* [FLT-APO-08] The peak has a row of its own in the flight log. */
void test_FLT_APO_08_the_peak_is_logged(void) {
    flight_conditions_t c = obeying(0);
    c.stop_after_apogee = false;
    c.to_landed = true;
    flown_t r = fly(&ROCKETS[SUBSONIC].r, &ISA, &c, 21, 400.0f);
    static char log[262144];
    TEST_ASSERT_TRUE(test_flight_log_csv(log, (int)sizeof(log)) > 0);
    const char *row = strstr(log, ",PEAK\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(row, "no PEAK row");
    while (row > log && row[-1] != '\n')
        row--;
    long t_ms, pa, cm;
    TEST_ASSERT_EQUAL_INT(3, sscanf(row, "%ld,%ld,%ld", &t_ms, &pa, &cm));
    TEST_ASSERT_EQUAL_INT32(r.peak_cm, cm);
    TEST_ASSERT_NULL_MESSAGE(strstr(log, "PEAK_AT_LEAST"), "the climb was seen");
}

/* ── Every estimator flown at once ────────────────────────────────── */

static int rows_with(const char *name, const char *what) {
    char want[48];
    snprintf(want, sizeof(want), "%s %s", name, what);
    int n = 0;
    for (int i = 0; i < mock_estimator_row_count; i++)
        n += strncmp(mock_estimator_rows[i], want, strlen(want)) == 0;
    return n;
}

/* [SNS-EST-06] The flight obeys the estimator config.ini names: its own
 * apogee is the flight's. */
void test_SNS_EST_06_the_flight_obeys_the_configured_estimator(void) {
    TEST_ASSERT_TRUE_MESSAGE(estimator_count() >= 2, "one estimator cannot show a choice");
    for (uint8_t e = 0; e < estimator_count(); e++) {
        flight_conditions_t c = obeying(e);
        c.port = PORT_READS_HIGH;
        flown_t r = fly(&ROCKETS[LOW_DRAG].r, &COLD, &c, 22, 120.0f);
        char msg[160];
        snprintf(msg, sizeof(msg), "obeying %s: declared %.2f s, its own %d at %.2f s", c.estimator,
                 (double)r.declared_t, r.said_apogee[e], (double)r.said_t[e]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(e, pp_obeyed(), msg);
        TEST_ASSERT_TRUE_MESSAGE(r.apogee_declared && r.said_apogee[e], msg);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.03f, r.said_t[e], r.declared_t, msg);
    }
}

/* [SNS-EST-06] A name no estimator has is the default's. */
void test_SNS_EST_06_an_unknown_name_is_the_default(void) {
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.estimator = "nosuch";
    c.stop_after_apogee = true;
    flown_t r = fly(&ROCKETS[HOP].r, &COLD, &c, 23, 60.0f);
    TEST_ASSERT_TRUE(r.launched);
    TEST_ASSERT_EQUAL_UINT8(0, pp_obeyed());
    TEST_ASSERT_EQUAL_UINT8(0, estimator_index(NULL));
}

/* [SNS-EST-07] Every estimator is flown on every reading, whichever is
 * obeyed: each says its own apogee in the log, and each reports once a
 * second from launch to landing. */
void test_SNS_EST_07_every_estimator_is_flown_and_logged(void) {
    flight_conditions_t c = obeying(0);
    c.stop_after_apogee = false;
    c.to_landed = true;
    flown_t r = fly(&ROCKETS[SUBSONIC].r, &COLD, &c, 24, 400.0f);
    TEST_ASSERT_TRUE(r.landed_t > 0.0f);
    for (uint8_t e = 0; e < estimator_count(); e++) {
        const char *name = estimator_at(e)->name;
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: %d APOGEE rows, %d reports in %.0f s; said apogee %+.2f s from the true one",
                 name, rows_with(name, "APOGEE"), rows_with(name, "cm="), (double)r.touchdown_t,
                 (double)(r.said_t[e] - r.apogee_t));
        printf("  %s\n", msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, rows_with(name, "APOGEE"), msg);
        TEST_ASSERT_INT_WITHIN_MESSAGE(3, (int)r.touchdown_t, rows_with(name, "cm="), msg);
        TEST_ASSERT_TRUE_MESSAGE(r.said_apogee[e] && r.said_t[e] >= r.apogee_t, msg);
    }
}

/* ── M2: sensor failure in flight ─────────────────────────────────── */

static bool log_has(const char *event) {
    static char log[65536];
    if (test_flight_log_csv(log, (int)sizeof(log)) <= 0)
        return false;
    char field[40];
    snprintf(field, sizeof(field), ",%s", event);
    return strstr(log, field) != NULL;
}

/* [SNS-PRES-10] A sensor that sticks in coast, and stays stuck, deploys
 * nothing: just after burnout, or half a second before apogee. */
void test_SNS_PRES_10_stuck_in_coast_deploys_nothing(void) {
    const unsigned profiles[] = {SUBSONIC, MID_MACH};
    char bad[256] = "";
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 2; i++) {
            const mp_rocket_t *rk = &ROCKETS[profiles[i]].r;
            flight_conditions_t c = obeying(e);
            c.stop_after_apogee = false;
            c.stuck_at_s = i ? rk->burn_s + 0.5f : plant_apogee_s(rk, &COLD) - 0.5f;
            (void)fly(rk, &COLD, &c, 12, 120.0f);
            if (mock_pyro.fire_count != 0) {
                char item[96];
                snprintf(item, sizeof(item), " %s %s: %d fires;", c.estimator, ROCKETS[profiles[i]].name,
                         mock_pyro.fire_count);
                note(bad, sizeof(bad), item);
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [SNS-PRES-11, FLT-APO-07] Half a second without samples, across the apogee:
 * nothing is decided until a second of new samples exists, and the climb to
 * this fall was not seen, so the fall must last. */
void test_SNS_PRES_11_a_gap_across_apogee_waits_for_new_samples(void) {
    const mp_rocket_t *rk = &ROCKETS[SUBSONIC].r;
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.dropout_at_s = plant_apogee_s(rk, &COLD) - 0.3f;
    c.dropout_s = 0.5f;
    flown_t r = fly(rk, &COLD, &c, 13, 120.0f);
    float back = c.dropout_at_s + c.dropout_s;
    char msg[128];
    snprintf(msg, sizeof(msg), "samples back at %.2f s, drogue at %.2f s, apogee at %.2f s", (double)back,
             (double)r.drogue_t, (double)r.apogee_t);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t >= back + 1.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t <= back + 3.5f, msg);
}

/* [SNS-PRES-11] Two seconds without samples under the drogue, the main set
 * where the rocket falls meanwhile: the loss is flagged, and the main waits
 * for a whole window of new samples rather than fire on the old ones. */
void test_SNS_PRES_11_lost_under_a_canopy_waits_for_new_samples(void) {
    const mp_rocket_t *rk = &ROCKETS[SUBSONIC].r;
    float ap_t = plant_apogee_s(rk, &ISA);
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.main_m = (uint16_t)(plant_apogee_m(rk, &ISA) - 100.0f);
    c.dropout_at_s = ap_t + 4.5f;
    c.dropout_s = 2.0f;
    flown_t r = fly(rk, &ISA, &c, 14, 120.0f);
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
void test_SNS_PRES_06_readings_the_part_cannot_output_are_discarded(void) {
    const mp_rocket_t *rk = &ROCKETS[SUBSONIC].r;
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.glitch_at_s = plant_apogee_s(rk, &COLD) - 1.0f;
    c.glitch_pa = 150000;
    c.glitch_n = 25;
    flown_t r = fly(rk, &COLD, &c, 15, 120.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "%u rejected; drogue %+.2f s from apogee", (unsigned)mock_pres_rejects,
             (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(mock_pres_rejects >= 25, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 3.5f, msg);
}

/* [SNS-PRES-10, SNS-PRES-11] Each failure is a DIAG bit, which /api/status
 * lists by name, and an event in the log. A sensor that comes back flies on:
 * stuck for 2 s, or gone for 1, a second or more before apogee. */
void test_SNS_PRES_10_a_failure_is_reported_and_the_flight_goes_on(void) {
    const mp_rocket_t *rk = &ROCKETS[SUBSONIC].r;
    float ap_t = plant_apogee_s(rk, &COLD);
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.stuck_at_s = ap_t - 4.0f;
    c.stuck_s = 2.0f;
    c.stop_after_apogee = true;
    flown_t r = fly(rk, &COLD, &c, 16, 120.0f);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_SENSOR_STUCK) != 0, "a stuck sensor sets its DIAG bit");
    TEST_ASSERT_EQUAL_STRING("sensor_stuck", pad_check_fault_name(DIAG_SENSOR_STUCK));
    TEST_ASSERT_TRUE_MESSAGE(log_has("SENSOR_STUCK"), "and logs it");
    char msg[96];
    snprintf(msg, sizeof(msg), "stuck: drogue %d, %+.2f s from apogee", r.drogue, (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 1.5f, msg);

    memset(&c, 0, sizeof(c));
    c.dropout_at_s = ap_t - 4.0f;
    c.dropout_s = 1.0f;
    c.stop_after_apogee = true;
    r = fly(rk, &COLD, &c, 17, 120.0f);
    TEST_ASSERT_TRUE_MESSAGE((ctx.diag & DIAG_SENSOR_LOST) != 0, "a lost sensor sets its DIAG bit");
    TEST_ASSERT_EQUAL_STRING("sensor_lost", pad_check_fault_name(DIAG_SENSOR_LOST));
    TEST_ASSERT_TRUE_MESSAGE(log_has("SENSOR_LOST"), "and logs it");
    snprintf(msg, sizeof(msg), "lost: drogue %d, %+.2f s from apogee", r.drogue, (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t && r.drogue_t - r.apogee_t <= 1.5f, msg);
}

/* [SNS-PRES-10] guard: a sensor with the MS5607's noise never reads as stuck,
 * over an hour on the pad. */
void test_SNS_PRES_10_a_working_sensor_is_never_stuck(void) {
    boot_like_hardware(18);
    uint32_t t = 0;
    run_to_pad(&t);
    for (uint32_t end = t + 3600000u; t < end; t++)
        tick(t);
    TEST_ASSERT_EQUAL(PAD_IDLE, ctx.current_state);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, ctx.diag & DIAG_SENSOR_STUCK, "an hour of real noise read as a stuck sensor");
}

/* ── T9: at the MS5607's ~90 Hz ───────────────────────────────────── */

/* The fast profiles, both pads, clean ports and the one that fakes a
 * descent: the drogue after apogee, sampled every 11 ms as at 20. */
void test_FLT_APO_07_the_same_at_ninety_readings_a_second(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_FAKES_DESCENT};
    char bad[1024] = "";
    for (uint8_t e = 0; e < estimator_count(); e++) {
        for (unsigned i = 0; i < 3; i++) {
            for (int si = 0; si < 2; si++) {
                for (unsigned pi = 0; pi < 2; pi++) {
                    flight_conditions_t c = obeying(e);
                    if (ports[pi])
                        c.port = *ports[pi];
                    c.interval_ms = 11u;
                    flown_t r = fly(&ROCKETS[fast[i]].r, SITES[si], &c, 20, 120.0f);
                    char what[64];
                    snprintf(what, sizeof(what), "%s %s %s%s", c.estimator, ROCKETS[fast[i]].name, si ? "hot" : "cold",
                             pi ? " fakes descent" : "");
                    drogue_after_apogee(&r, what, 1.5f, bad, sizeof(bad));
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_TST_03_plant_atmosphere);
    RUN_TEST(test_TST_03_plant_mach);
    RUN_TEST(test_TST_03_plant_port_error);
    RUN_TEST(test_TST_03_plant_charge);
    RUN_TEST(test_TST_03_plant_sensor_failures);
    RUN_TEST(test_TST_03_plant_profiles);
    RUN_TEST(test_TST_03_report);
    RUN_TEST(test_PYR_MODE_06_a_charge_in_the_bay_does_not_move_the_main);
    RUN_TEST(test_SNS_EST_03_a_swinging_canopy_moves_neither_the_main_nor_the_landing);
    RUN_TEST(test_FLT_APO_07_a_subsonic_flight_deploys_at_apogee);
    RUN_TEST(test_FLT_ASC_08_nothing_arms_below_30_m);
    RUN_TEST(test_FLT_APO_07_no_fire_before_apogee_when_supersonic);
    RUN_TEST(test_FLT_APO_07_no_fire_before_apogee_at_any_size_of_port_error);
    RUN_TEST(test_FLT_APO_07_at_10_km_on_every_seed);
    RUN_TEST(test_FLT_APO_07_a_fall_whose_climb_was_not_seen_must_last);
    RUN_TEST(test_FLT_APO_08_the_peak_is_from_what_the_estimator_explained);
    RUN_TEST(test_FLT_APO_08_the_peak_is_logged);
    RUN_TEST(test_SNS_EST_06_the_flight_obeys_the_configured_estimator);
    RUN_TEST(test_SNS_EST_06_an_unknown_name_is_the_default);
    RUN_TEST(test_SNS_EST_07_every_estimator_is_flown_and_logged);
    RUN_TEST(test_SNS_PRES_10_stuck_in_coast_deploys_nothing);
    RUN_TEST(test_SNS_PRES_11_a_gap_across_apogee_waits_for_new_samples);
    RUN_TEST(test_SNS_PRES_11_lost_under_a_canopy_waits_for_new_samples);
    RUN_TEST(test_SNS_PRES_06_readings_the_part_cannot_output_are_discarded);
    RUN_TEST(test_SNS_PRES_10_a_failure_is_reported_and_the_flight_goes_on);
    RUN_TEST(test_SNS_PRES_10_a_working_sensor_is_never_stuck);
    RUN_TEST(test_FLT_APO_07_the_same_at_ninety_readings_a_second);
    return UNITY_END();
}
