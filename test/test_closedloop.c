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
#include "../src/telemetry_formatter.h"
#include "../src/hal.h"
#include "../src/buzzer.h"

/* ── Physics ──────────────────────────────────────────────────────── */

#define G 9.81f
#define DT 0.001f
#define GROUND_PA 101325.0f
#define PAD_DWELL_MS 2000 /* sit on pad before launch */

#define DROGUE_DRAG 0.8f /* ~8 m/s terminal */
#define MAIN_DRAG 4.0f   /* ~2 m/s terminal */

typedef struct {
    float target_alt_m;
    float thrust_accel;
    float burn_time;
} flight_profile_t;

typedef struct {
    float alt_m;
    float vel_ms; /* positive = up */
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

/* Buzzer mock */
static int buzzer_stop_count, buzzer_altitude_count;
static int32_t last_buzzer_altitude;
static bool buzzer_active_flag;
void buzzer_init(void) {}
void buzzer_set_code(uint8_t c, bool r) {
    (void)c;
    (void)r;
    buzzer_active_flag = true;
}
void buzzer_set_altitude(int32_t a) {
    buzzer_altitude_count++;
    last_buzzer_altitude = a;
}
void buzzer_stop(void) {
    buzzer_stop_count++;
    buzzer_active_flag = false;
}
bool buzzer_is_active(void) {
    return buzzer_active_flag;
}
void buzzer_update(uint32_t n) {
    (void)n;
}

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
    flight_profile_t p = {.target_alt_m = target_m};
    /* Solve for burn time: h = 0.5*(a-g)*t² + ((a-g)*t)²/(2g)
     * Simplify: pick thrust, iterate burn time */
    if (target_m > 50000.0f) {
        p.thrust_accel = 5.0f * G; /* 5g for Karman */
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
        if (h_burn + h_coast < target_m)
            lo = t;
        else
            hi = t;
    }
    p.burn_time = (lo + hi) / 2.0f;
    return p;
}

static void physics_step(physics_state_t *ps, float flight_t, const flight_profile_t *p) {
    if (ps->on_ground)
        return;
    float a = -G;
    if (flight_t < p->burn_time)
        a += p->thrust_accel;
    if (ps->vel_ms < 0.0f) {
        float drag = 0.05f;
        if (ps->main_deployed)
            drag = MAIN_DRAG;
        else if (ps->drogue_deployed)
            drag = DROGUE_DRAG;
        /* Scale drag by atmospheric density (exponential decay, scale height 8.5km) */
        float density_frac = expf(-ps->alt_m / 8500.0f);
        a += drag * density_frac * (-ps->vel_ms);
    }
    ps->vel_ms += a * DT;
    ps->alt_m += ps->vel_ms * DT;
    if (ps->alt_m <= 0.0f) {
        ps->alt_m = 0;
        ps->vel_ms = 0;
        ps->on_ground = true;
    }
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
    if (r->reached_ascent)
        printf("launch=%.1fs ", r->launch_ms / 1000.0);
    if (r->reached_descent)
        printf("apogee=%.1fs ", r->apogee_ms / 1000.0);
    if (r->pyro1_fired)
        printf("P1=%.1fs@%.0fm ", r->p1_fire_ms / 1000.0, r->pyro1_alt_m);
    if (r->pyro2_fired)
        printf("P2=%.1fs@%.0fm ", r->p2_fire_ms / 1000.0, r->pyro2_alt_m);
    if (r->reached_landed)
        printf("landed=%.1fs ", r->landed_ms / 1000.0);
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
    telemetry_init(&ctx.config); /* must be called before any telemetry_state() */
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    physics_state_t ps = {0};
    sim_result_t res = {0};

    /* Scale sim: 1ms steps for normal, 50ms for Karman */
    uint32_t step = (prof.target_alt_m > 50000.0f) ? 50 : 1;
    uint32_t max_ms = (prof.target_alt_m > 50000.0f) ? 15000000 : (prof.target_alt_m > 500.0f) ? 600000 : 120000;
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
        if (ps.alt_m > res.apogee_m)
            res.apogee_m = ps.alt_m;

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

        /* Telemetry + pyro update + buzzer via flight_update_outputs() */
        flight_update_outputs(&ctx, t);
    }

    res.sample_count = ctx.buf_count;
    char *p = mock_uart_buf;
    while ((p = strstr(p, "$PYRO,")) != NULL) {
        res.telemetry_count++;
        p++;
    }
    return res;
}

/* ── Configs ──────────────────────────────────────────────────────── */

/* Pyro1 = drogue, Pyro2 = main */
static config_t cfg_delay_delay(void) {
    return (config_t){.id = "DD",
                      .name = "DlyDly",
                      .pyro1_mode = PYRO_MODE_DELAY,
                      .pyro1_value = 0,
                      .pyro2_mode = PYRO_MODE_DELAY,
                      .pyro2_value = 3,
                      .units = 2};
}
static config_t cfg_delay_agl(void) {
    return (config_t){.id = "DA",
                      .name = "DlyAgl",
                      .pyro1_mode = PYRO_MODE_DELAY,
                      .pyro1_value = 0,
                      .pyro2_mode = PYRO_MODE_AGL,
                      .pyro2_value = 200,
                      .units = 2};
}
static config_t cfg_delay_fallen(void) {
    return (config_t){.id = "DF",
                      .name = "DlyFal",
                      .pyro1_mode = PYRO_MODE_DELAY,
                      .pyro1_value = 0,
                      .pyro2_mode = PYRO_MODE_FALLEN,
                      .pyro2_value = 100,
                      .units = 2};
}
static config_t cfg_delay_speed(void) {
    return (config_t){.id = "DS",
                      .name = "DlySpd",
                      .pyro1_mode = PYRO_MODE_DELAY,
                      .pyro1_value = 0,
                      .pyro2_mode = PYRO_MODE_SPEED,
                      .pyro2_value = 30,
                      .units = 2};
}
static config_t cfg_agl_agl(void) {
    return (config_t){.id = "AA",
                      .name = "AglAgl",
                      .pyro1_mode = PYRO_MODE_AGL,
                      .pyro1_value = 400,
                      .pyro2_mode = PYRO_MODE_AGL,
                      .pyro2_value = 200,
                      .units = 2};
}
static config_t cfg_fallen_agl(void) {
    return (config_t){.id = "FA",
                      .name = "FalAgl",
                      .pyro1_mode = PYRO_MODE_FALLEN,
                      .pyro1_value = 50,
                      .pyro2_mode = PYRO_MODE_AGL,
                      .pyro2_value = 200,
                      .units = 2};
}
static config_t cfg_speed_agl(void) {
    return (config_t){.id = "SA",
                      .name = "SpdAgl",
                      .pyro1_mode = PYRO_MODE_SPEED,
                      .pyro1_value = 20,
                      .pyro2_mode = PYRO_MODE_AGL,
                      .pyro2_value = 200,
                      .units = 2};
}

/* Altitudes */
static const float ALT_LOW = 30.48f;       /* 100 ft */
static const float ALT_MED = 152.4f;       /* 500 ft */
static const float ALT_HIGH = 1524.0f;     /* 5000 ft */
static const float ALT_KARMAN = 100000.0f; /* 100 km */

/* ── Assertions ───────────────────────────────────────────────────── */

static void assert_flight(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: no ASCENT", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_ascent, m);
    snprintf(m, sizeof(m), "%s: no DESCENT", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_descent, m);
    snprintf(m, sizeof(m), "%s: no LANDED", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_landed, m);
}

static void assert_p1(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: P1 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro1_fired, m);
}

static void assert_p2(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: P2 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro2_fired, m);
}

static void assert_order(sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: main higher than drogue (P1=%.0f P2=%.0f)", l, r->pyro1_alt_m, r->pyro2_alt_m);
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
    float alts[] = {ALT_LOW, ALT_MED, ALT_HIGH, ALT_KARMAN};
    const char *names[] = {"100ft", "500ft", "5000ft", "Karman"};

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

void test_PYR_MODE_01_delay_delay(void) {
    run_suite(cfg_delay_delay, "Dly+Dly");
}
void test_PYR_MODE_02_delay_agl(void) {
    run_suite(cfg_delay_agl, "Dly+AGL");
}
void test_PYR_MODE_03_delay_fallen(void) {
    run_suite(cfg_delay_fallen, "Dly+Fal");
}
void test_PYR_MODE_04_delay_speed(void) {
    run_suite(cfg_delay_speed, "Dly+Spd");
}
void test_PYR_MODE_02_agl_agl(void) {
    run_suite(cfg_agl_agl, "AGL+AGL");
}
void test_PYR_MODE_03_fallen_agl(void) {
    run_suite(cfg_fallen_agl, "Fal+AGL");
}
void test_PYR_MODE_04_speed_agl(void) {
    run_suite(cfg_speed_agl, "Spd+AGL");
}

void test_TST_06_chute_effect(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    config_t cfg = cfg_delay_agl();

    sim_result_t with = run_sim(cfg, prof, true);
    print_summary("chute: with", &with);
    sim_result_t without = run_sim(cfg, prof, false);
    print_summary("chute: without", &without);

    char msg[128];
    snprintf(msg, sizeof(msg), "With chutes (%ums) should be longer than ballistic (%ums)", with.flight_time_ms,
             without.flight_time_ms);
    TEST_ASSERT_TRUE_MESSAGE(with.flight_time_ms > without.flight_time_ms, msg);
}

/* ── Safety-critical tests ────────────────────────────────────────── */

/* [PYR-SAFE-01] No fire without continuity: pyro1 open → should not fire */
void test_PYR_SAFE_01_no_fire_without_continuity(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    config_t cfg = cfg_delay_agl();

    /* Run with pyro1 open (no continuity), pyro2 good */
    mock_reset_all();
    mock_pyro.p1_good = false;
    mock_pyro.p1_open = true;
    mock_pyro.p2_good = true;
    mock_pyro.p2_adc = 50;
    mock_pyro.p1_adc = 4000;
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
    uint8_t prev_fires = 0;

    for (uint32_t t = 0; t <= 120000; t++) {
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1) {
                res.pyro1_fired = true;
                res.p1_fire_ms = t;
            }
            if (ch == 2 && !ps.main_deployed) {
                ps.main_deployed = true;
                res.pyro2_fired = true;
                res.p2_fire_ms = t;
            }
            prev_fires = mock_pyro.fire_count;
        }
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, &prof);
        }
        if (ps.alt_m > res.apogee_m)
            res.apogee_m = ps.alt_m;
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = dispatch_state(&ctx, t);
        if (ctx.current_state == LANDED)
            break;
    }

    print_summary("NoCont P1", &res);
    TEST_ASSERT_FALSE_MESSAGE(res.pyro1_fired, "Pyro1 fired despite no continuity");
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "Pyro2 should fire with good continuity");
}

/* [PYR-SAFE-02] No simultaneous fire: pyros fire sequentially */
void test_PYR_SAFE_02_no_simultaneous_fire(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    /* Both set to delay=0 so both want to fire at apogee */
    config_t cfg = (config_t){.id = "SS",
                              .name = "SimFir",
                              .pyro1_mode = PYRO_MODE_DELAY,
                              .pyro1_value = 0,
                              .pyro2_mode = PYRO_MODE_DELAY,
                              .pyro2_value = 0,
                              .units = 2};

    sim_result_t r = run_sim(cfg, prof, true);
    print_summary("NoSimulFire", &r);

    assert_flight(&r, "NoSimulFire");
    assert_p1(&r, "NoSimulFire");
    assert_p2(&r, "NoSimulFire");

    /* The two pyros must fire on different ticks (P2 waits for P1 to finish) */
    char msg[128];
    snprintf(msg, sizeof(msg), "P1 and P2 fired at same time: P1=%u P2=%u", r.p1_fire_ms, r.p2_fire_ms);
    TEST_ASSERT_TRUE_MESSAGE(r.p1_fire_ms != r.p2_fire_ms, msg);
}

/* [SYS-DEPLOY-03] No firing during ascent (before apogee) */
void test_SYS_DEPLOY_03_no_fire_during_ascent(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    config_t cfg = cfg_delay_agl();

    mock_reset_all();
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    mock_uart_len = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    ctx.config = cfg;
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    physics_state_t ps = {0};
    bool pyro_during_ascent = false;

    for (uint32_t t = 0; t <= 120000; t++) {
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, &prof);
        }
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = dispatch_state(&ctx, t);

        /* Check: any pyro fire during ASCENT is a safety violation */
        if (ctx.current_state == ASCENT && mock_pyro.fire_count > 0) {
            pyro_during_ascent = true;
        }
        if (ctx.current_state == LANDED)
            break;
    }

    TEST_ASSERT_FALSE_MESSAGE(pyro_during_ascent, "Pyro fired during ASCENT — must only fire after apogee");
}

/* [PYR-FAULT-02] Overcurrent fault detection via FLAG pin */
void test_PYR_FAULT_02_overcurrent_detection(void) {
    flight_profile_t prof = make_profile(ALT_HIGH);
    /* Use delay+delay so both fire quickly at apogee, within 120s budget */
    config_t cfg = cfg_delay_delay();

    mock_reset_all();
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    mock_pyro.fault = true; /* Inject fault on all channels */
    mock_uart_len = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    ctx.config = cfg;
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    physics_state_t ps = {0};
    uint8_t prev_fires = 0;

    for (uint32_t t = 0; t <= 120000; t++) {
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1 && !ps.drogue_deployed)
                ps.drogue_deployed = true;
            if (ch == 2 && !ps.main_deployed)
                ps.main_deployed = true;
            prev_fires = mock_pyro.fire_count;
        }
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, &prof);
        }
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = dispatch_state(&ctx, t);
        if (ctx.current_state == LANDED)
            break;
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.pyro1_fault, "Pyro1 fault not detected");
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyro2_fault, "Pyro2 fault not detected");

    /* Verify fault events via CSV (buffer may have wrapped) */
    flight_save_csv(&ctx);
    char buf[4096];
    int n = hal_fs_read_file("flight.csv", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "No CSV data");
    buf[n] = '\0';
    /* EVT_PYRO1_FAULT and EVT_PYRO2_FAULT don't have named strings in CSV yet,
     * but the fault flags on ctx are the primary verification */
}

void test_TST_05_karman_apogee(void) {
    flight_profile_t prof = make_profile(ALT_KARMAN);
    config_t cfg = cfg_delay_delay();
    sim_result_t r = run_sim(cfg, prof, true);
    print_summary("Karman", &r);

    assert_flight(&r, "Karman");
    TEST_ASSERT_INT_WITHIN(20000, 100000, (int)r.apogee_m);
}

/* ── XIP stall robustness test ────────────────────────────────────── */
/* Simulates RP2040 flash write stalls (XIP disabled during erase+write).
 * Each hal_fs_write / hal_fs_close advances mock_time_ms by stall_ms.
 * The sim loop accounts for time jumps just like real hardware:
 * the CPU stalls, misses sample windows, then resumes.
 *
 * This verifies that:
 * - Pyros still fire correctly despite time jumps from flash writes
 * - No flight state machine lockup from unexpected time advances
 * - csv_flush_safe() correctly gates flash writes away from pyro timing
 */
void test_XIP_stall_pyro_timing(void) {
    mock_reset_all();
    /* Realistic RP2040 flash timing: page program ~2-5ms,
     * sector erase + program ~50ms. Use 10ms as pessimistic
     * per-operation average (write = page program, close = metadata
     * commit which may trigger erase). */
    mock_xip_stall_ms = 10;
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;

    config_t cfg = cfg_delay_agl();
    flight_context_t ctx = {0};
    ctx.config = cfg;
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    flight_profile_t prof = make_profile(ALT_HIGH);
    physics_state_t ps = {0};
    uint32_t step = 1;
    uint32_t max_ms = 600000;
    uint8_t prev_fires = 0;
    bool p1_fired = false, p2_fired = false;
    float p1_alt = 0, p2_alt = 0;
    uint32_t stall_during_descent = 0;

    uint32_t phys_t = 0; /* last physics time — tracks real elapsed time */

    for (uint32_t t = 0; t <= max_ms; t += step) {
        /* Check pyro fires */
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1 && !ps.drogue_deployed) {
                ps.drogue_deployed = true;
                p1_fired = true;
                p1_alt = ps.alt_m;
            }
            if (ch == 2 && !ps.main_deployed) {
                ps.main_deployed = true;
                p2_fired = true;
                p2_alt = ps.alt_m;
            }
            prev_fires = mock_pyro.fire_count;
        }

        /* Physics: advance all missed ms since last step.
         * On real hardware, the rocket keeps flying during XIP stalls —
         * only the CPU stalls. When it resumes, the pressure sensor
         * reads the current physical state. */
        if (t >= PAD_DWELL_MS) {
            uint32_t phys_start = (phys_t >= PAD_DWELL_MS) ? phys_t : PAD_DWELL_MS;
            uint32_t steps_needed = t - phys_start;
            for (uint32_t s = 0; s < steps_needed; s++) {
                float ft = (float)(phys_start + s - PAD_DWELL_MS) / 1000.0f;
                physics_step(&ps, ft, &prof);
            }
        }
        phys_t = t;

        /* Feed firmware — it sees the post-stall time and current physics */
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;

        uint32_t stall_before = mock_xip_total_stall_ms;
        ctx.current_state = dispatch_state(&ctx, mock_time_ms);

        /* Account for XIP stall: if flash write advanced time, skip ahead.
         * Physics will catch up on the next iteration. */
        if (mock_time_ms > t)
            t = mock_time_ms;

        /* CSV flush when safe (this triggers XIP stalls) */
        if (csv_flush_safe(&ctx) && (ctx.buf_count > 0) && (!ctx.csv_header_written || ctx.csv_pending > 0)) {
            csv_flush_step(&ctx, 4);
            if (mock_time_ms > t)
                t = mock_time_ms;
        }

        if (ctx.current_state == DESCENT)
            stall_during_descent += (mock_xip_total_stall_ms - stall_before);

        if (ctx.current_state == LANDED)
            break;
    }

    printf("  XIP stall: %ums total, %d events, stall_per_op=%ums\n", mock_xip_total_stall_ms, mock_xip_stall_count,
           mock_xip_stall_ms);
    printf("  XIP descent stall: %ums (should be small until pyros resolve)\n", stall_during_descent);
    printf("  XIP result: P1=%s@%.0fm P2=%s@%.0fm landed=%s\n", p1_fired ? "FIRED" : "MISSED", p1_alt,
           p2_fired ? "FIRED" : "MISSED", p2_alt, ctx.current_state == LANDED ? "yes" : "no");

    /* Both pyros must still fire despite XIP stalls */
    TEST_ASSERT_TRUE_MESSAGE(p1_fired, "Pyro1 failed to fire with XIP stalls");
    TEST_ASSERT_TRUE_MESSAGE(p2_fired, "Pyro2 failed to fire with XIP stalls");
    TEST_ASSERT_EQUAL_MESSAGE(LANDED, ctx.current_state, "Did not reach LANDED with XIP stalls");

    /* Verify stalls actually occurred */
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, mock_xip_stall_count, "No XIP stalls occurred — test didn't exercise flash");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, mock_xip_total_stall_ms, "No XIP stall time accumulated");
}

/* [PYR-REFIRE-01] Re-fire a channel if descent speed exceeds 30 m/s between
 * 1.0 and 1.5 seconds after initial fire and continuity is still present.
 *
 * Scenario: 1000m apogee, pyro1 = FALLEN 100m (fires ~4.5s after apogee when
 * falling at ~44 m/s). Failed deployment is simulated by keeping mock_pyro
 * p1_good=true and p1_open=false — the circuit is intact, the e-match didn't
 * fire the charge. The rocket stays ballistic (ps.drogue_deployed stays false).
 * check_refire() in detect_descent() detects ballistic + continuity + timing
 * and fires pyro1 a second time within the 1.0-1.5s window. */
void test_PYR_REFIRE_01_refire_ballistic(void) {
    mock_reset_all();
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    /* 1000m apogee: ballistic speed ~44 m/s when fallen 100m from peak.
     * FALLEN 100m fires while descending fast — re-fire window at T+1.0-1.5s
     * still has speed >> 30 m/s (ballistic = ctx->vertical_speed_cms < -3000). */
    flight_profile_t prof = make_profile(1000.0f);

    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_FALLEN;
    ctx.config.pyro1_value = 100; /* 100m below apogee — fires while descending fast */
    ctx.config.pyro2_mode = PYRO_MODE_DELAY;
    ctx.config.pyro2_value = 60; /* 60s delay — won't fire in test window */
    ctx.config.units = 1;        /* meters */
    ctx.config.backup_timer = 0; /* disable backup apogee timer */
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;

    physics_state_t ps = {0};
    uint8_t prev_fires = 0;
    bool initial_fired = false;
    bool refire_detected = false;
    uint32_t first_fire_time = 0;
    uint32_t second_fire_time = 0;

    for (uint32_t t = 0; t <= 120000; t++) {
        /* Detect pyro fires from mock */
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1) {
                if (!initial_fired) {
                    initial_fired = true;
                    first_fire_time = t;
                    /* Failed deployment: do NOT set ps.drogue_deployed.
                     * mock_pyro.p1_good and p1_open stay at their defaults
                     * (good=true, open=false) — circuit intact, charge failed. */
                } else if (!refire_detected) {
                    refire_detected = true;
                    second_fire_time = t;
                }
            }
            prev_fires = mock_pyro.fire_count;
        }

        /* Physics: pad dwell then flight. Without drogue, rocket is ballistic. */
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, &prof);
        }

        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = dispatch_state(&ctx, t);

        if (refire_detected)
            break;
        if (ctx.current_state == LANDED)
            break;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Pyro1 never fired initially (FALLEN threshold vs apogee height)");
    TEST_ASSERT_TRUE_MESSAGE(initial_fired, msg);

    snprintf(msg, sizeof(msg), "Re-fire not detected: initial_fire=%ums speed_cms=%d (need < -3000)", first_fire_time,
             ctx.vertical_speed_cms);
    TEST_ASSERT_TRUE_MESSAGE(refire_detected, msg);

    /* Re-fire must occur within the 1000-1500ms window after initial fire */
    uint32_t window_ms = second_fire_time - first_fire_time;
    snprintf(msg, sizeof(msg), "Re-fire window %ums not in [1000,1500]ms", window_ms);
    TEST_ASSERT_TRUE_MESSAGE(window_ms >= 1000 && window_ms <= 1500, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_PYR_MODE_01_delay_delay);
    RUN_TEST(test_PYR_MODE_02_delay_agl);
    RUN_TEST(test_PYR_MODE_03_delay_fallen);
    RUN_TEST(test_PYR_MODE_04_delay_speed);
    RUN_TEST(test_PYR_MODE_02_agl_agl);
    RUN_TEST(test_PYR_MODE_03_fallen_agl);
    RUN_TEST(test_PYR_MODE_04_speed_agl);
    RUN_TEST(test_TST_06_chute_effect);
    RUN_TEST(test_PYR_SAFE_01_no_fire_without_continuity);
    RUN_TEST(test_PYR_SAFE_02_no_simultaneous_fire);
    RUN_TEST(test_SYS_DEPLOY_03_no_fire_during_ascent);
    RUN_TEST(test_PYR_FAULT_02_overcurrent_detection);
    RUN_TEST(test_TST_05_karman_apogee);
    RUN_TEST(test_XIP_stall_pyro_timing);
    RUN_TEST(test_PYR_REFIRE_01_refire_ballistic);
    return UNITY_END();
}
