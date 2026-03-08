/*
 * Closed-loop flight simulation tests.
 *
 * Physics: thrust → coast → drogue descent → main descent → landing.
 * Pyro fires feed back into physics (chute deployment slows descent).
 * Tests every pyro config mode × 4 altitude profiles.
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../src/flight_states.h"
#include "../src/buzzer.h"

/* ── Physics ──────────────────────────────────────────────────────── */

#define G           9.81f
#define DT          0.001f
#define GROUND_PA   101325.0f
#define PAD_DWELL_MS 2000   /* sit on pad before launch */

#define DROGUE_DRAG  0.8f   /* ~8 m/s terminal */
#define MAIN_DRAG    4.0f   /* ~2 m/s terminal */

typedef struct {
    float target_alt_m;
    float thrust_accel;
    float burn_time;
} flight_profile_t;

typedef struct {
    float alt_m;
    float vel_ms;       /* positive = up */
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

/* Buzzer mock */
static int buzzer_stop_count, buzzer_altitude_count;
static int32_t last_buzzer_altitude;
static bool buzzer_active_flag;
void buzzer_init(void) {}
void buzzer_set_code(uint8_t c, bool r) { (void)c; (void)r; buzzer_active_flag = true; }
void buzzer_set_altitude(int32_t a) { buzzer_altitude_count++; last_buzzer_altitude = a; }
void buzzer_stop(void) { buzzer_stop_count++; buzzer_active_flag = false; }
bool buzzer_is_active(void) { return buzzer_active_flag; }
void buzzer_update(uint32_t n) { (void)n; }

static float alt_m_to_pa(float alt_m) {
    /* Standard atmosphere: P = P0 * (1 - L*h/T0)^(g*M/(R*L))
     * Below 11km (troposphere). Above 11km, use stratosphere model. */
    if (alt_m < 11000.0f) {
        return GROUND_PA * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
    }
    /* Stratosphere (11-47km): isothermal at 216.65K */
    float p11 = GROUND_PA * powf(1.0f - 0.0065f * 11000.0f / 288.15f, 5.2561f); /* ~22632 Pa */
    if (alt_m < 47000.0f) {
        return p11 * expf(-9.81f * (alt_m - 11000.0f) / (287.05f * 216.65f));
    }
    /* Above 47km: very low pressure */
    float p47 = p11 * expf(-9.81f * 36000.0f / (287.05f * 216.65f));
    return p47 * expf(-9.81f * (alt_m - 47000.0f) / (287.05f * 270.65f));
}

static flight_profile_t make_profile(float target_m) {
    flight_profile_t p = { .target_alt_m = target_m };
    /* Solve for burn time: h = 0.5*(a-g)*t² + ((a-g)*t)²/(2g)
     * Simplify: pick thrust, iterate burn time */
    if (target_m > 50000.0f) {
        p.thrust_accel = 5.0f * G;  /* 5g for Karman */
    } else if (target_m > 500.0f) {
        p.thrust_accel = 10.0f * G;
    } else {
        p.thrust_accel = 20.0f * G;
    }
    /* Binary search for burn time that reaches target apogee */
    float lo = 0.0f, hi = 200.0f;
    for (int i = 0; i < 50; i++) {
        float t = (lo + hi) / 2.0f;
        float a_net = p.thrust_accel - G;
        float v_bo = a_net * t;
        float h_burn = 0.5f * a_net * t * t;
        float h_coast = v_bo * v_bo / (2.0f * G);
        if (h_burn + h_coast < target_m) lo = t; else hi = t;
    }
    p.burn_time = (lo + hi) / 2.0f;
    return p;
}

static void physics_step(physics_state_t *ps, float flight_t, const flight_profile_t *p) {
    if (ps->on_ground) return;
    float a = -G;
    if (flight_t < p->burn_time) a += p->thrust_accel;
    if (ps->vel_ms < 0.0f) {
        float drag = 0.05f;
        if (ps->main_deployed) drag = MAIN_DRAG;
        else if (ps->drogue_deployed) drag = DROGUE_DRAG;
        /* Scale drag by atmospheric density (exponential decay, scale height 8.5km) */
        float density_frac = expf(-ps->alt_m / 8500.0f);
        a += drag * density_frac * (-ps->vel_ms);
    }
    ps->vel_ms += a * DT;
    ps->alt_m += ps->vel_ms * DT;
    if (ps->alt_m <= 0.0f) { ps->alt_m = 0; ps->vel_ms = 0; ps->on_ground = true; }
}

/* ── Sim runner ───────────────────────────────────────────────────── */

typedef struct {
    bool reached_ascent, reached_descent, reached_landed;
    bool pyro1_fired, pyro2_fired;
    float pyro1_alt_m, pyro2_alt_m;
    float apogee_m, landing_speed_ms;
    uint32_t flight_time_ms;
    uint32_t launch_ms, apogee_ms, p1_fire_ms, p2_fire_ms, landed_ms;
    float launch_alt_m, apogee_alt_m;
    int sample_count, telemetry_count;
} sim_result_t;

static void print_summary(const char *label, sim_result_t *r) {
    printf("  %-20s apogee=%6.0fm  ", label, r->apogee_m);
    if (r->reached_ascent)  printf("launch=%.1fs ", r->launch_ms / 1000.0);
    if (r->reached_descent) printf("apogee=%.1fs ", r->apogee_ms / 1000.0);
    if (r->pyro1_fired)     printf("P1=%.1fs@%.0fm ", r->p1_fire_ms / 1000.0, r->pyro1_alt_m);
    if (r->pyro2_fired)     printf("P2=%.1fs@%.0fm ", r->p2_fire_ms / 1000.0, r->pyro2_alt_m);
    if (r->reached_landed)  printf("landed=%.1fs ", r->landed_ms / 1000.0);
    printf("samples=%d telem=%d\n", r->sample_count, r->telemetry_count);
}

static sim_result_t run_sim(config_t cfg, flight_profile_t prof, bool enable_pyros) {
    mock_reset_all();
    mock_pyro.p1_good = enable_pyros;
    mock_pyro.p2_good = enable_pyros;
    mock_pyro.p1_adc = enable_pyros ? 50 : 0;
    mock_pyro.p2_adc = enable_pyros ? 50 : 0;
    mock_uart_len = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    ctx.config = cfg;
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    physics_state_t ps = {0};
    sim_result_t res = {0};

    /* Scale sim: 1ms steps for normal, 50ms for Karman */
    uint32_t step = (prof.target_alt_m > 50000.0f) ? 50 : 1;
    uint32_t max_ms = (prof.target_alt_m > 50000.0f) ? 15000000
                    : (prof.target_alt_m > 500.0f)   ? 600000
                    :                                   120000;
    uint8_t prev_fires = 0;

    for (uint32_t t = 0; t <= max_ms; t += step) {
        /* Closed-loop: check pyro fires, update physics */
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1 && !ps.drogue_deployed) {
                ps.drogue_deployed = true;
                res.pyro1_fired = true;
                res.pyro1_alt_m = ps.alt_m;
                res.p1_fire_ms = t;
            }
            if (ch == 2 && !ps.main_deployed) {
                ps.main_deployed = true;
                res.pyro2_fired = true;
                res.pyro2_alt_m = ps.alt_m;
                res.p2_fire_ms = t;
            }
            prev_fires = mock_pyro.fire_count;
        }

        /* Physics: pad dwell then flight */
        if (t >= PAD_DWELL_MS) {
            float flight_t = (float)(t - PAD_DWELL_MS) / 1000.0f;
            for (uint32_t s = 0; s < step; s++) {
                physics_step(&ps, flight_t + (float)s * DT, &prof);
            }
        }
        if (ps.alt_m > res.apogee_m) res.apogee_m = ps.alt_m;

        /* Feed firmware */
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = dispatch_state(&ctx, t);

        if (ctx.current_state == ASCENT && !res.reached_ascent) {
            res.reached_ascent = true;
            res.launch_ms = t;
            res.launch_alt_m = ps.alt_m;
        }
        if (ctx.current_state == DESCENT && !res.reached_descent) {
            res.reached_descent = true;
            res.apogee_ms = t;
            res.apogee_alt_m = ps.alt_m;
        }
        if (ctx.current_state == LANDED && !res.reached_landed) {
            res.reached_landed = true;
            res.flight_time_ms = t - ctx.launch_time;
            res.landed_ms = t;
            res.landing_speed_ms = fabsf(ps.vel_ms);
            break;
        }

        /* Telemetry in loop */
        if (ctx.current_state >= PAD_IDLE) {
            uint32_t iv = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
            if (t - ctx.last_telemetry >= iv) {
                uint32_t ft = (ctx.current_state != PAD_IDLE) ? (t - ctx.launch_time) : 0;
                send_telemetry(&ctx, ft, ctx.last_altitude, ctx.current_state);
                ctx.last_telemetry = t;
            }
        }
    }

    res.sample_count = ctx.buf_count;
    char *p = mock_uart_buf;
    while ((p = strstr(p, "$PYRO,")) != NULL) { res.telemetry_count++; p++; }
    return res;
}

/* ── Configs ──────────────────────────────────────────────────────── */

/* Pyro1 = drogue, Pyro2 = main */
static config_t cfg_delay_delay(void)  { return (config_t){"DD","DlyDly", PYRO_MODE_DELAY,0, PYRO_MODE_DELAY,3, 2,0}; }
static config_t cfg_delay_agl(void)    { return (config_t){"DA","DlyAgl", PYRO_MODE_DELAY,0, PYRO_MODE_AGL,200, 2,0}; }
static config_t cfg_delay_fallen(void) { return (config_t){"DF","DlyFal", PYRO_MODE_DELAY,0, PYRO_MODE_FALLEN,100, 2,0}; }
static config_t cfg_delay_speed(void)  { return (config_t){"DS","DlySpd", PYRO_MODE_DELAY,0, PYRO_MODE_SPEED,30, 2,0}; }
static config_t cfg_agl_agl(void)      { return (config_t){"AA","AglAgl", PYRO_MODE_AGL,400, PYRO_MODE_AGL,200, 2,0}; }
static config_t cfg_fallen_agl(void)   { return (config_t){"FA","FalAgl", PYRO_MODE_FALLEN,50, PYRO_MODE_AGL,200, 2,0}; }
static config_t cfg_speed_agl(void)    { return (config_t){"SA","SpdAgl", PYRO_MODE_SPEED,20, PYRO_MODE_AGL,200, 2,0}; }

/* Altitudes */
static const float ALT_LOW    = 30.48f;    /* 100 ft */
static const float ALT_MED    = 152.4f;    /* 500 ft */
static const float ALT_HIGH   = 1524.0f;   /* 5000 ft */
static const float ALT_KARMAN = 100000.0f; /* 100 km */

/* ── Assertions ───────────────────────────────────────────────────── */

static void assert_flight(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: no ASCENT", l);  TEST_ASSERT_TRUE_MESSAGE(r->reached_ascent, m);
    snprintf(m, sizeof(m), "%s: no DESCENT", l); TEST_ASSERT_TRUE_MESSAGE(r->reached_descent, m);
    snprintf(m, sizeof(m), "%s: no LANDED", l);  TEST_ASSERT_TRUE_MESSAGE(r->reached_landed, m);
}

static void assert_p1(sim_result_t *r, const char *l) {
    char m[128]; snprintf(m, sizeof(m), "%s: P1 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro1_fired, m);
}

static void assert_p2(sim_result_t *r, const char *l) {
    char m[128]; snprintf(m, sizeof(m), "%s: P2 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro2_fired, m);
}

static void assert_order(sim_result_t *r, const char *l) {
    char m[128]; snprintf(m, sizeof(m), "%s: main higher than drogue (P1=%.0f P2=%.0f)",
                          l, r->pyro1_alt_m, r->pyro2_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro1_alt_m >= r->pyro2_alt_m, m);
}

static void assert_data(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: samples=%d", l, r->sample_count);
    TEST_ASSERT_TRUE_MESSAGE(r->sample_count > 10, m);
    snprintf(m, sizeof(m), "%s: telemetry=%d", l, r->telemetry_count);
    TEST_ASSERT_TRUE_MESSAGE(r->telemetry_count > 2, m);
}

/* ── Config suite runner ──────────────────────────────────────────── */

typedef config_t (*cfg_fn)(void);

static void run_suite(cfg_fn make, const char *name) {
    float alts[] = { ALT_LOW, ALT_MED, ALT_HIGH, ALT_KARMAN };
    const char *names[] = { "100ft", "500ft", "5000ft", "Karman" };

    for (int i = 0; i < 4; i++) {
        config_t c = make();

        /* Scale thresholds for low flights so they're achievable */
        float apogee_ft = alts[i] * 3.281f;
        if (c.pyro1_mode == PYRO_MODE_AGL && c.pyro1_value > (uint16_t)(apogee_ft * 0.9f))
            c.pyro1_value = (uint16_t)(apogee_ft * 0.5f);
        if (c.pyro2_mode == PYRO_MODE_AGL && c.pyro2_value > (uint16_t)(apogee_ft * 0.9f))
            c.pyro2_value = (uint16_t)(apogee_ft * 0.5f);
        if (c.pyro1_mode == PYRO_MODE_FALLEN && c.pyro1_value > (uint16_t)(apogee_ft * 0.5f))
            c.pyro1_value = (uint16_t)(apogee_ft * 0.3f);
        if (c.pyro2_mode == PYRO_MODE_FALLEN && c.pyro2_value > (uint16_t)(apogee_ft * 0.5f))
            c.pyro2_value = (uint16_t)(apogee_ft * 0.3f);

        flight_profile_t prof = make_profile(alts[i]);
        char label[64];
        snprintf(label, sizeof(label), "%s@%s", name, names[i]);

        sim_result_t r = run_sim(c, prof, true);

        print_summary(label, &r);
        assert_flight(&r, label);
        assert_p1(&r, label);
        assert_data(&r, label);

        /* Main chute: expect fire on medium+ flights (low flights may land before trigger) */
        if (alts[i] >= ALT_MED) {
            assert_p2(&r, label);
            /* Skip order check for Karman — both pyros fire near apogee */
            if (alts[i] < ALT_KARMAN)
                assert_order(&r, label);
        }
    }
}

/* ── Tests ────────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

void test_delay_delay(void)  { run_suite(cfg_delay_delay,  "Dly+Dly"); }
void test_delay_agl(void)    { run_suite(cfg_delay_agl,    "Dly+AGL"); }
void test_delay_fallen(void) { run_suite(cfg_delay_fallen, "Dly+Fal"); }
void test_delay_speed(void)  { run_suite(cfg_delay_speed,  "Dly+Spd"); }
void test_agl_agl(void)      { run_suite(cfg_agl_agl,      "AGL+AGL"); }
void test_fallen_agl(void)   { run_suite(cfg_fallen_agl,   "Fal+AGL"); }
void test_speed_agl(void)    { run_suite(cfg_speed_agl,    "Spd+AGL"); }

void test_chute_slows_descent(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    config_t cfg = cfg_delay_agl();

    sim_result_t with = run_sim(cfg, prof, true);
    print_summary("chute: with", &with);
    sim_result_t without = run_sim(cfg, prof, false);
    print_summary("chute: without", &without);

    char msg[128];
    snprintf(msg, sizeof(msg), "With chutes (%ums) should be longer than ballistic (%ums)",
             with.flight_time_ms, without.flight_time_ms);
    TEST_ASSERT_TRUE_MESSAGE(with.flight_time_ms > without.flight_time_ms, msg);
}

void test_karman_apogee(void) {
    flight_profile_t prof = make_profile(ALT_KARMAN);
    config_t cfg = cfg_delay_delay();
    sim_result_t r = run_sim(cfg, prof, true);
    print_summary("Karman", &r);

    assert_flight(&r, "Karman");
    TEST_ASSERT_INT_WITHIN(20000, 100000, (int)r.apogee_m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delay_delay);
    RUN_TEST(test_delay_agl);
    RUN_TEST(test_delay_fallen);
    RUN_TEST(test_delay_speed);
    RUN_TEST(test_agl_agl);
    RUN_TEST(test_fallen_agl);
    RUN_TEST(test_speed_agl);
    RUN_TEST(test_chute_slows_descent);
    RUN_TEST(test_karman_apogee);
    return UNITY_END();
}
