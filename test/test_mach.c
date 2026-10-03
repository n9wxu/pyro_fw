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
#include "../src/mach_lockout.h"
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

/* ── M0: the report, today ────────────────────────────────────────── */

/* For each profile, pad and port: did the drogue fire, when against the true
 * apogee, and where the Mach gate let go. What M1 must change is read here. */
void test_TST_03_report(void) {
    const mp_site_t *sites[] = {&COLD, &HOT};
    const struct {
        const char *name;
        const mp_port_t *port;
    } ports[] = {{"clean", NULL},
                 {"reads high", &PORT_READS_HIGH},
                 {"reads low", &PORT_READS_LOW},
                 {"fakes descent", &PORT_FAKES_DESCENT}};
    printf("  %-9s %-5s %-13s %-9s %-17s %s\n", "profile", "pad", "ports", "max Mach", "drogue", "lock release");
    for (unsigned i = 0; i < N_ROCKETS; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 4; pi++) {
                if (pi > 0 && i < DRAGGY && i != BOOST_30G)
                    continue; /* subsonic flights see no port error */
                flight_conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi].port)
                    c.port = *ports[pi].port;
                flown_t r = fly(&ROCKETS[i].r, sites[si], &c, 7, 120.0f);
                TEST_ASSERT_TRUE_MESSAGE(r.launched, ROCKETS[i].name);
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
                printf("  %-9s %2.0f C  %-13s %-9.2f %-17s %s\n", ROCKETS[i].name, sites[si]->temp_c, ports[pi].name,
                       r.max_mach, drogue, release);
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

/* ── M1: the Mach lockout ─────────────────────────────────────────── */

static const mp_site_t *const SITES[] = {&COLD, &HOT};

static void drogue_after_apogee(const flown_t *r, const char *what, char *bad, size_t cap) {
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
void test_FLT_MACH_02_a_subsonic_flight_is_never_flagged(void) {
    char bad[256] = "";
    for (int si = 0; si < 2; si++) {
        flight_conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        flown_t r = fly(&ROCKETS[SUBSONIC].r, SITES[si], &c, 3, 120.0f);
        if (r.locked)
            strncat(bad, si ? " locked at the hot pad;" : " locked at the cold pad;", sizeof(bad) - 1 - strlen(bad));
        drogue_after_apogee(&r, si ? "hot" : "cold", bad, sizeof(bad));
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-02] The flag is set while the data is still clean: before Mach
 * 0.85, where a port's error begins -- on a 30 g boost too, whose ignition
 * step spoils the fits for the first second. */
void test_FLT_MACH_02_flagged_before_mach_085(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    char bad[256] = "";
    float worst = 0.0f;
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            flight_conditions_t c;
            memset(&c, 0, sizeof(c));
            flown_t r = fly(&ROCKETS[fast[i]].r, SITES[si], &c, 4, ROCKETS[fast[i]].r.burn_s + 2.0f);
            worst = r.locked && r.flag_mach > worst ? r.flag_mach : worst;
            if (!r.locked || r.flag_mach >= 0.85f) {
                char item[64];
                snprintf(item, sizeof(item), " %s %s: %s Mach %.2f;", ROCKETS[fast[i]].name, si ? "hot" : "cold",
                         r.locked ? "flagged at" : "never flagged, peak", (double)(r.locked ? r.flag_mach : r.max_mach));
                strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
            }
        }
    }
    printf("  flagged by Mach %.2f at the latest\n", (double)worst);
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-03] A peak between the flag and Mach 0.85 is flagged for nothing,
 * and let go soon after burnout: a second for the burnout's step to pass,
 * the slowing below the release speed, then a second of the coast's
 * signature. Within 4 s, and well before apogee. */
void test_FLT_MACH_03_a_mid_mach_flight_releases_after_burnout(void) {
    const mp_rocket_t *rk = &ROCKETS[MID_MACH].r;
    char bad[256] = "";
    int flagged = 0;
    for (int si = 0; si < 2; si++) {
        flight_conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        flown_t r = fly(rk, SITES[si], &c, 5, 120.0f);
        drogue_after_apogee(&r, si ? "hot" : "cold", bad, sizeof(bad));
        if (!r.locked)
            continue;
        flagged++;
        printf("  mid-Mach %s: flagged at Mach %.2f, released %.2f s after burnout at Mach %.2f\n", si ? "hot" : "cold",
               (double)r.flag_mach, (double)(r.release_t - rk->burn_s), (double)r.release_mach);
        if (!r.released || r.release_t - rk->burn_s > 4.0f || r.apogee_t - r.release_t < 5.0f) {
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
void test_FLT_MACH_05_no_fire_before_apogee_when_supersonic(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_READS_HIGH, &PORT_READS_LOW, &PORT_FAKES_DESCENT};
    const char *names[] = {"clean", "high", "low", "fakes descent"};
    char bad[512] = "";
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 4; pi++) {
                flight_conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi])
                    c.port = *ports[pi];
                c.stop_after_apogee = true;
                flown_t r = fly(&ROCKETS[fast[i]].r, SITES[si], &c, 6, 120.0f);
                char what[48];
                snprintf(what, sizeof(what), "%s %s %s", ROCKETS[fast[i]].name, si ? "hot" : "cold", names[pi]);
                drogue_after_apogee(&r, what, bad, sizeof(bad));
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [FLT-MACH-03] At altitude the release margin is thinnest: the low-drag
 * flight to 10 km from the hot, high pad releases before apogee on every
 * seed. */
void test_FLT_MACH_03_released_before_apogee_at_10_km(void) {
    int late = 0, never = 0;
    float least = 1e9f;
    for (uint32_t seed = 1; seed <= 1000; seed++) {
        flight_conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stop_after_apogee = true;
        flown_t r = fly(&ROCKETS[LOW_DRAG].r, &HOT, &c, seed, 120.0f);
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

/* [FLT-MACH-04] No readings from just after burnout until half a second
 * before apogee: the flag cannot be released. Apogee is still found, from
 * the rocket falling as gravity gives, seconds after the true apogee, and not
 * a minute later when it has fallen back to where the flag was set. The peak
 * is marked a lower bound [FLT-MACH-07]. */
void test_FLT_MACH_04_apogee_is_found_while_the_flag_stands(void) {
    const mp_rocket_t *rk = &ROCKETS[MID_MACH].r;
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.dropout_at_s = rk->burn_s + 0.5f;
    c.dropout_s = plant_apogee_s(rk, &COLD) - 0.5f - c.dropout_at_s;
    flown_t r = fly(rk, &COLD, &c, 8, 200.0f);
    char msg[200];
    snprintf(msg, sizeof(msg),
             "flagged %d released %d found under the flag %d; apogee %.2f s, drogue %.2f s, back past the flag %.2f s",
             r.locked, r.released, r.fallback, (double)r.apogee_t, (double)r.drogue_t, (double)r.return_t);
    printf("  no readings through the coast: drogue %.2f s after apogee, %.1f s before the flag's level\n",
           (double)(r.drogue_t - r.apogee_t), (double)(r.return_t - r.drogue_t));
    TEST_ASSERT_TRUE_MESSAGE(r.locked && !r.released && r.fallback, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue_t - r.apogee_t <= 5.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.return_t == 0.0f || r.drogue_t < r.return_t, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.peak_lower_bound, msg);
}

/* [FLT-MACH-07] The reported peak is the lowest filtered pressure outside
 * the flag: a port that reads low while supersonic reads the rocket
 * kilometres high, and none of it may reach the report. A lock that let go
 * within 2 s of apogee marks the peak a lower bound. On the standard
 * atmosphere's pad, where pressure altitude is the true height. */
void test_FLT_MACH_07_the_peak_is_from_outside_the_flag(void) {
    const mp_port_t reads_very_low = {-1.0f, 0.06f, 0.15f, 0.09f};
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.port = reads_very_low;
    c.stop_after_apogee = true;
    flown_t r = fly(&ROCKETS[DRAGGY].r, &ISA, &c, 9, 120.0f);
    char msg[128];
    snprintf(msg, sizeof(msg), "reported %.0f m, true apogee %.0f m, lower bound %d", r.peak_cm / 100.0,
             (double)r.apogee_h, r.peak_lower_bound);
    printf("  draggy, a port reading 15%% of q low: %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(r.peak_cm / 100.0 - (double)r.apogee_h) <= 5.0, msg);
    TEST_ASSERT_FALSE_MESSAGE(r.peak_lower_bound, msg);

    /* No readings until 3.3 s before apogee: the release comes inside the
     * last 2 s. */
    const mp_rocket_t *mm = &ROCKETS[MID_MACH].r;
    memset(&c, 0, sizeof(c));
    c.dropout_at_s = mm->burn_s + 0.5f;
    c.dropout_s = plant_apogee_s(mm, &ISA) - 5.0f - c.dropout_at_s;
    c.stop_after_apogee = true;
    r = fly(mm, &ISA, &c, 10, 120.0f);
    snprintf(msg, sizeof(msg), "released %d, %.2f s before apogee, lower bound %d", r.released,
             (double)(r.apogee_t - r.release_t), r.peak_lower_bound);
    TEST_ASSERT_TRUE_MESSAGE(r.released && r.apogee_t - r.release_t < 2.0f, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.peak_lower_bound, msg);
}

/* [FLT-MACH-02..06] Each threshold, a hair either side. */
void test_FLT_MACH_02_thresholds(void) {
    TEST_ASSERT_TRUE(mach_too_fast(-0.0291f));
    TEST_ASSERT_FALSE(mach_too_fast(-0.0289f));
    TEST_ASSERT_FALSE_MESSAGE(mach_too_fast(0.05f), "a descent is not too fast a climb");
    TEST_ASSERT_TRUE(mach_slow_ascent(-0.0219f));
    TEST_ASSERT_FALSE(mach_slow_ascent(-0.0221f));
    TEST_ASSERT_FALSE(mach_slow_ascent(0.001f));
    TEST_ASSERT_TRUE(mach_slow_descent(0.0219f));
    TEST_ASSERT_FALSE(mach_slow_descent(0.0221f));
    TEST_ASSERT_FALSE(mach_slow_descent(-0.001f));
    TEST_ASSERT_TRUE(mach_under_gravity(0.0f, 0.00091f));
    TEST_ASSERT_FALSE(mach_under_gravity(0.0f, 0.00089f));
    TEST_ASSERT_TRUE_MESSAGE(mach_under_gravity(0.02f, 0.0006f), "the rate's own square counts");
    TEST_ASSERT_TRUE(mach_above_arm_height(0.9964f * 101325.0f, 101325.0f));
    TEST_ASSERT_FALSE(mach_above_arm_height(0.9966f * 101325.0f, 101325.0f));
}

/* What a port's error must do to fake the release: the fakes-descent shape
 * scaled, both signs, on the fast profiles. Reported for
 * docs/mach_lockout.md's risks. The lock must never let go before burnout or
 * while the port's error is live, and the drogue must never come early. */
void test_FLT_MACH_03_no_release_inside_a_port_error(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const float scales[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    char bad[512] = "";
    float worst = 0.0f;
    printf("  release Mach, by port error (x fakes-descent), cold pad:\n");
    for (unsigned i = 0; i < 3; i++) {
        char line[200];
        snprintf(line, sizeof(line), "    %-9s", ROCKETS[fast[i]].name);
        for (int sign = -1; sign <= 1; sign += 2) {
            for (unsigned k = 0; k < sizeof(scales) / sizeof(scales[0]); k++) {
                flight_conditions_t c;
                memset(&c, 0, sizeof(c));
                c.port = PORT_FAKES_DESCENT;
                c.port.sign = (float)sign;
                c.port.below *= scales[k];
                c.port.above *= scales[k];
                c.port.slope *= scales[k];
                c.stop_after_apogee = true;
                flown_t r = fly(&ROCKETS[fast[i]].r, &COLD, &c, 11, 120.0f);
                float m = fabsf(r.release_mach);
                worst = r.released && m > worst ? m : worst;
                size_t l = strlen(line);
                snprintf(line + l, sizeof(line) - l, " %+.2g:%.2f", (double)(sign * scales[k]), (double)m);
                char what[48];
                snprintf(what, sizeof(what), "%s x%+.2g", ROCKETS[fast[i]].name, (double)(sign * scales[k]));
                if (r.released && (r.release_t < ROCKETS[fast[i]].r.burn_s || m >= 0.85f)) {
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
    if (test_flight_log_csv(log, (int)sizeof(log)) <= 0)
        return false;
    char field[40];
    snprintf(field, sizeof(field), ",%s", event);
    return strstr(log, field) != NULL;
}

/* [SNS-PRES-10] A sensor that sticks in coast, and stays stuck, deploys
 * nothing: armed on the subsonic flight, locked on the mid-Mach one. */
void test_SNS_PRES_10_stuck_in_coast_deploys_nothing(void) {
    const struct {
        unsigned prof;
        bool locked;
    } cases[] = {{SUBSONIC, false}, {MID_MACH, true}};
    char bad[256] = "";
    for (unsigned i = 0; i < 2; i++) {
        const mp_rocket_t *rk = &ROCKETS[cases[i].prof].r;
        flight_conditions_t c;
        memset(&c, 0, sizeof(c));
        c.stuck_at_s = cases[i].locked ? rk->burn_s + 0.5f : plant_apogee_s(rk, &COLD) - 0.5f;
        flown_t r = fly(rk, &COLD, &c, 12, 120.0f);
        bool state_ok = cases[i].locked ? ctx.mach.flagged || !r.released : ctx.pyros_armed;
        if (mock_pyro.fire_count != 0 || !state_ok) {
            char item[96];
            snprintf(item, sizeof(item), " %s: %d fires, %s;", ROCKETS[cases[i].prof].name, mock_pyro.fire_count,
                     state_ok ? "as it should be" : (cases[i].locked ? "not locked" : "not armed"));
            strncat(bad, item, sizeof(bad) - 1 - strlen(bad));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(bad[0] == '\0', bad);
}

/* [SNS-PRES-11] Half a second without samples, across the apogee: no fit is
 * clean until a whole window of new samples exists, so the apogee waits for
 * it. */
void test_SNS_PRES_11_a_gap_across_apogee_waits_for_new_samples(void) {
    const mp_rocket_t *rk = &ROCKETS[SUBSONIC].r;
    flight_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.dropout_at_s = plant_apogee_s(rk, &COLD) - 0.3f;
    c.dropout_s = 0.5f;
    c.stop_after_apogee = true;
    flown_t r = fly(rk, &COLD, &c, 13, 120.0f);
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
    c.stop_after_apogee = true;
    flown_t r = fly(rk, &COLD, &c, 15, 120.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "%u rejected; drogue %+.2f s from apogee", (unsigned)mock_pres_rejects,
             (double)(r.drogue_t - r.apogee_t));
    TEST_ASSERT_TRUE_MESSAGE(mock_pres_rejects >= 25, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.drogue && r.drogue_t >= r.apogee_t, msg);
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

/* ── T9: the lockout at the MS5607's ~90 Hz ───────────────────────── */

/* The fast profiles, both pads, clean ports and the one that fakes a
 * descent: flagged before Mach 0.85, released before apogee, and the drogue
 * after it, sampled every 11 ms as at 20. */
void test_FLT_MACH_03_the_same_at_ninety_readings_a_second(void) {
    const unsigned fast[] = {DRAGGY, LOW_DRAG, BOOST_30G};
    const mp_port_t *ports[] = {NULL, &PORT_FAKES_DESCENT};
    char bad[512] = "";
    for (unsigned i = 0; i < 3; i++) {
        for (int si = 0; si < 2; si++) {
            for (unsigned pi = 0; pi < 2; pi++) {
                flight_conditions_t c;
                memset(&c, 0, sizeof(c));
                if (ports[pi])
                    c.port = *ports[pi];
                c.stop_after_apogee = true;
                c.interval_ms = 11u;
                flown_t r = fly(&ROCKETS[fast[i]].r, SITES[si], &c, 20, 120.0f);
                char what[48];
                snprintf(what, sizeof(what), "%s %s%s", ROCKETS[fast[i]].name, si ? "hot" : "cold",
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
    RUN_TEST(test_TST_03_plant_atmosphere);
    RUN_TEST(test_TST_03_plant_mach);
    RUN_TEST(test_TST_03_plant_port_error);
    RUN_TEST(test_TST_03_plant_charge);
    RUN_TEST(test_TST_03_plant_sensor_failures);
    RUN_TEST(test_TST_03_plant_profiles);
    RUN_TEST(test_TST_03_report);
    RUN_TEST(test_PYR_MODE_06_a_charge_in_the_bay_does_not_move_the_main);
    RUN_TEST(test_SNS_EST_03_a_swinging_canopy_moves_neither_the_main_nor_the_landing);
    RUN_TEST(test_FLT_MACH_02_a_subsonic_flight_is_never_flagged);
    RUN_TEST(test_FLT_MACH_02_flagged_before_mach_085);
    RUN_TEST(test_FLT_MACH_03_a_mid_mach_flight_releases_after_burnout);
    RUN_TEST(test_FLT_MACH_05_no_fire_before_apogee_when_supersonic);
    RUN_TEST(test_FLT_MACH_03_released_before_apogee_at_10_km);
    RUN_TEST(test_FLT_MACH_04_apogee_is_found_while_the_flag_stands);
    RUN_TEST(test_FLT_MACH_07_the_peak_is_from_outside_the_flag);
    RUN_TEST(test_FLT_MACH_02_thresholds);
    RUN_TEST(test_FLT_MACH_03_no_release_inside_a_port_error);
    RUN_TEST(test_SNS_PRES_10_stuck_in_coast_deploys_nothing);
    RUN_TEST(test_SNS_PRES_11_a_gap_across_apogee_waits_for_new_samples);
    RUN_TEST(test_SNS_PRES_11_lost_under_a_canopy_waits_for_new_samples);
    RUN_TEST(test_SNS_PRES_06_readings_the_part_cannot_output_are_discarded);
    RUN_TEST(test_SNS_PRES_10_a_failure_is_reported_and_the_flight_goes_on);
    RUN_TEST(test_SNS_PRES_10_a_working_sensor_is_never_stuck);
    RUN_TEST(test_FLT_MACH_03_the_same_at_ninety_readings_a_second);
    return UNITY_END();
}
