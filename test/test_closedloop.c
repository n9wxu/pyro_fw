/*
 * Closed-loop flight simulation tests.
 *
 * Rocket profiles are loaded from test_data/rockets.json at startup.
 * Each profile describes a real motor (standard NAR/NFPA nomenclature: A8-3,
 * D12-5, H73-8, …) together with the rocket's dry mass and aerodynamic
 * parameters.  The physics engine converts those into force-based kinematics
 * with a variable-mass propellant model.
 *
 * Physics: rectangular-average thrust → coast → drogue descent → main descent.
 * Pyro fires feed back into physics (chute deployment changes descent rate).
 *
 * Motor nomenclature: X##-D
 *   X  = impulse class (A=2.5 N·s, B=5, C=10, D=20, E=40, F=80, G=160, H=320 N·s)
 *   ## = average thrust (N)
 *   D  = ejection-charge delay after burnout (s)  [not used in physics]
 *
 * Data sources:
 *   Motor specs – thrustcurve.org
 *   Rocket kits – Estes, AeroTech, apogeerockets.com product listings
 */
#include "unity.h"
#include "mocks.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/flight_states.h"
#include "pressure_processing.h"
#include "../src/telemetry_formatter.h"
#include "../src/hal.h"
#include "../src/buzzer.h"

/* Wrapper: feed pressure into pp, then dispatch */
static flight_state_t step(flight_context_t *ctx, uint32_t now) {
    hal_tasks_tick(now);
    return dispatch_state(ctx, now);
}

/* ── Constants ────────────────────────────────────────────────────── */

#define G 9.81f
#define DT 0.001f /* physics step (s) = 1 ms */
#define GROUND_PA 101325.0f
#define PAD_DWELL_MS 2000 /* sit on pad before ignition */

/* Parachute drag-deceleration coefficients (empirical, not physical Cd·A)
 * Terminal velocity: v_term = G / CHUTE_DRAG
 * DROGUE_DRAG = 9.81/12 ≈ 0.82  → ~12 m/s drogue terminal
 * MAIN_DRAG   = 9.81/2  ≈ 4.9   → ~2 m/s main terminal          */
#define DROGUE_DRAG 0.8f
#define MAIN_DRAG 4.0f

/* JSON loader limits */
#define MAX_ROCKETS 8
#define MAX_NAME_LEN 80
#define MAX_MOTOR_LEN 16
#define JSON_BUF_SIZE 8192

/* ── Rocket profile ───────────────────────────────────────────────── */

typedef struct {
    char name[MAX_NAME_LEN];   /* "Estes Alpha III on A8-3" */
    char motor[MAX_MOTOR_LEN]; /* "A8-3" */
    float dry_mass_kg;         /* airframe + avionics, no propellant */
    float prop_mass_kg;        /* propellant consumed during burn */
    float avg_thrust_n;        /* average thrust over burn (rectangular approx) */
    float burn_time_s;         /* motor burn duration */
    float cd;                  /* rocket drag coefficient */
    float ref_area_m2;         /* cross-sectional reference area */
    float expected_apogee_m;   /* nominal apogee (used for threshold scaling) */
    float apogee_tol;          /* fractional tolerance, e.g. 0.50 = ±50 % */
} rocket_profile_t;

/* Convenience indices — must match rockets.json order */
#define ROCKET_IDX_LOW 0  /* low power, A8-3  ~65 m  */
#define ROCKET_IDX_MID1 1 /* low-mid,   C6-5  ~175 m */
#define ROCKET_IDX_MID2 2 /* mid,       D12-5 ~420 m */
#define ROCKET_IDX_L1 3   /* HPR L1,    H73-8 ~950 m */

static rocket_profile_t g_rockets[MAX_ROCKETS];
static int g_num_rockets = 0;

/* ── Minimal JSON field extractor ─────────────────────────────────── */
/* Operates on a bounded substring [obj, obj+len) so field names in    */
/* one array element don't accidentally match in a different element.  */

static int json_float(const char *obj, size_t len, const char *key, float *out) {
    char token[MAX_NAME_LEN + 4];
    snprintf(token, sizeof(token), "\"%s\"", key);
    const char *end = obj + len;
    const char *p = obj;
    while (p < end) {
        const char *hit = strstr(p, token);
        if (!hit || hit >= end)
            return 0;
        p = hit + strlen(token);
        while (p < end && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        char *ep = NULL;
        float val = strtof(p, &ep);
        if (ep != p) {
            *out = val;
            return 1;
        }
    }
    return 0;
}

static int json_str(const char *obj, size_t len, const char *key, char *out, size_t outlen) {
    char token[MAX_NAME_LEN + 4];
    snprintf(token, sizeof(token), "\"%s\"", key);
    const char *end = obj + len;
    const char *hit = strstr(obj, token);
    if (!hit || hit >= end)
        return 0;
    const char *p = hit + strlen(token);
    while (p < end && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (p < end && *p != '"' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* Parse the "rockets" array from a JSON buffer.
 * Returns the number of rockets successfully loaded. */
static int load_rockets_json(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  [rockets.json] cannot open '%s'\n", path);
        return 0;
    }
    char buf[JSON_BUF_SIZE];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Locate the "rockets" array */
    const char *arr = strstr(buf, "\"rockets\"");
    if (!arr) {
        printf("  [rockets.json] no 'rockets' key\n");
        return 0;
    }
    arr = strchr(arr, '[');
    if (!arr) {
        printf("  [rockets.json] no '[' after rockets\n");
        return 0;
    }
    arr++;

    g_num_rockets = 0;
    const char *p = arr;
    while (*p && *p != ']' && g_num_rockets < MAX_ROCKETS) {
        /* Advance to next object */
        while (*p && *p != '{' && *p != ']')
            p++;
        if (*p != '{')
            break;
        const char *obj_start = p + 1;

        /* Find matching '}' (single-level — our schema is flat) */
        const char *q = obj_start;
        int depth = 1;
        while (*q && depth > 0) {
            if (*q == '{')
                depth++;
            else if (*q == '}')
                depth--;
            q++;
        }
        if (depth != 0)
            break;
        size_t obj_len = (size_t)(q - obj_start - 1);

        rocket_profile_t *r = &g_rockets[g_num_rockets];
        memset(r, 0, sizeof(*r));

        json_str(obj_start, obj_len, "name", r->name, sizeof(r->name));
        json_str(obj_start, obj_len, "motor", r->motor, sizeof(r->motor));

        float dry_g = 0, prop_g = 0, diam_mm = 0, tol_pct = 50.0f;
        json_float(obj_start, obj_len, "dry_mass_g", &dry_g);
        json_float(obj_start, obj_len, "prop_mass_g", &prop_g);
        json_float(obj_start, obj_len, "avg_thrust_n", &r->avg_thrust_n);
        json_float(obj_start, obj_len, "burn_time_s", &r->burn_time_s);
        json_float(obj_start, obj_len, "cd", &r->cd);
        json_float(obj_start, obj_len, "diameter_mm", &diam_mm);
        json_float(obj_start, obj_len, "expected_apogee_m", &r->expected_apogee_m);
        json_float(obj_start, obj_len, "apogee_tolerance_pct", &tol_pct);

        r->dry_mass_kg = dry_g / 1000.0f;
        r->prop_mass_kg = prop_g / 1000.0f;
        float radius_m = (diam_mm / 2.0f) / 1000.0f;
        r->ref_area_m2 = 3.14159f * radius_m * radius_m;
        r->apogee_tol = tol_pct / 100.0f;

        if (r->name[0] && r->avg_thrust_n > 0.0f && r->burn_time_s > 0.0f && r->dry_mass_kg > 0.0f) {
            printf(
                "  [rocket %d] %-45s  motor=%-8s  m=%.0fg+%.0fg  T=%.1fN  t=%.2fs  Cd=%.2f  d=%.0fmm  apogee~%.0fm\n",
                g_num_rockets, r->name, r->motor, dry_g, prop_g, r->avg_thrust_n, r->burn_time_s, r->cd, diam_mm,
                r->expected_apogee_m);
            g_num_rockets++;
        }
        p = q;
    }
    return g_num_rockets;
}

/* ── Buzzer mock ──────────────────────────────────────────────────── */

static int buzzer_stop_count;
static int buzzer_altitude_count;
static int32_t last_buzzer_altitude;
static bool buzzer_active_flag;

void buzzer_init(void) {}
/* beep_say() plays a spec now, because a chirp is not a code. Records the
 * outcome's sound the same way the code fake does. */
void buzzer_play_spec(const beep_spec_t *spec, uint16_t gap_ms, uint8_t repeat_count) {
    (void)spec;
    (void)gap_ms;
    (void)repeat_count;
    buzzer_active_flag = true;
}

void buzzer_play_altitude(int32_t a) {
    buzzer_altitude_count++;
    last_buzzer_altitude = a;
}
void buzzer_play_usb_ok(void) {
    buzzer_active_flag = true;
}
void buzzer_stop(void) {
    buzzer_stop_count++;
    buzzer_active_flag = false;
}
bool buzzer_is_active(void) {
    return buzzer_active_flag;
}

/* ── Atmosphere ───────────────────────────────────────────────────── */

static float alt_m_to_pa(float alt_m) {
    /* ISA troposphere (0–11 km) */
    if (alt_m < 11000.0f)
        return GROUND_PA * powf(1.0f - 0.0065f * alt_m / 288.15f, 5.2561f);
    /* Stratosphere (11–47 km): isothermal 216.65 K */
    float p11 = GROUND_PA * powf(1.0f - 0.0065f * 11000.0f / 288.15f, 5.2561f);
    if (alt_m < 47000.0f)
        return p11 * expf(-9.81f * (alt_m - 11000.0f) / (287.05f * 216.65f));
    float p47 = p11 * expf(-9.81f * 36000.0f / (287.05f * 216.65f));
    return p47 * expf(-9.81f * (alt_m - 47000.0f) / (287.05f * 270.65f));
}

/* ── Physics ──────────────────────────────────────────────────────── */

typedef struct {
    float alt_m;
    float vel_ms; /* positive = up */
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

/*
 * Integrate one 1-ms physics step using the rocket motor data.
 *
 * During burn:
 *   mass(t) = dry + prop * (1 - t/burn_time)   — linear propellant consumption
 *   F_thrust = avg_thrust_n
 *   F_drag   = ½·ρ(h)·v|v|·Cd·A                — opposes motion both ways
 *   a = (F_thrust - F_drag - mass·g) / mass
 *
 * After burnout (coast / descent without chute):
 *   same model with F_thrust = 0, mass = dry_mass
 *
 * With parachute (drogue or main):
 *   empirical chute drag: a = -g + DRAG·(ρ/ρ₀)·(-v)
 *   terminal velocity = g/DRAG  ≈ 12 m/s drogue, 2.5 m/s main
 */
static void physics_step(physics_state_t *ps, float flight_t, const rocket_profile_t *r) {
    if (ps->on_ground)
        return;

    float rho = 1.225f * expf(-ps->alt_m / 8500.0f); /* ISA density */
    float density_frac = rho / 1.225f;

    float a;
    if (ps->main_deployed || ps->drogue_deployed) {
        /* ── Parachute descent ── */
        float chute_drag = ps->main_deployed ? MAIN_DRAG : DROGUE_DRAG;
        a = -G + chute_drag * density_frac * (-ps->vel_ms);
    } else {
        /* ── Powered or ballistic flight ── */
        float prop_frac = (flight_t < r->burn_time_s) ? (1.0f - flight_t / r->burn_time_s) : 0.0f;
        float mass_kg = r->dry_mass_kg + r->prop_mass_kg * prop_frac;
        float thrust_n = (flight_t < r->burn_time_s) ? r->avg_thrust_n : 0.0f;

        /* Drag: v·|v| preserves sign so drag always opposes motion */
        float v_signed = ps->vel_ms;
        float f_drag = 0.5f * rho * v_signed * fabsf(v_signed) * r->cd * r->ref_area_m2;

        a = (thrust_n - f_drag - mass_kg * G) / mass_kg;
    }

    ps->vel_ms += a * DT;
    ps->alt_m += ps->vel_ms * DT;
    if (ps->alt_m <= 0.0f) {
        ps->alt_m = 0.0f;
        ps->vel_ms = 0.0f;
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
    /* For the emergency-ladder and phase tests: how many times each channel
     * was commanded, when the true apogee was (as against when the firmware
     * decided it was), the peak speed, and which phases were visited. */
    int p1_fires, p2_fires;
    uint32_t p1_refire_ms, apogee_true_ms;
    float max_speed_ms;
    bool saw_drogue_phase, saw_chute_phase;
    bool main_forced; /* the emergency ladder overrode pyro2's trigger */
} sim_result_t;

/* What the airframe does with a command. A canopy that is commanded and does
 * not open is the whole subject of the emergency ladder, so the sim has to be
 * able to refuse one. */
typedef struct {
    bool enable_pyros;     /* continuity present on both channels */
    bool drogue_works;     /* a commanded drogue actually opens */
    bool main_works;       /* a commanded main actually opens */
    bool p1_opens_on_fire; /* channel goes open-circuit, i.e. the charge lit */
} sim_opts_t;

static void print_summary(const char *label, const sim_result_t *r) {
    printf("  %-28s apogee=%6.0fm  ", label, r->apogee_m);
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

static sim_result_t run_sim_opts(config_t cfg, const rocket_profile_t *r, sim_opts_t o) {
    mock_reset_all();
    mock_pyro.p1_good = o.enable_pyros;
    mock_pyro.p2_good = o.enable_pyros;
    mock_pyro.p1_adc = o.enable_pyros ? 50 : 0;
    mock_pyro.p2_adc = o.enable_pyros ? 50 : 0;
    mock_uart_len = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    ctx.config = cfg;
    telemetry_init(&ctx.config);
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    sim_result_t res = {0};

    /* Time budget: 2 min for tiny low-power rockets, 10 min otherwise */
    uint32_t max_ms = (r->expected_apogee_m < 100.0f) ? 120000u : 600000u;
    uint8_t prev_fires = 0;

    for (uint32_t t = 0; t <= max_ms; t++) {
        /* Closed-loop: check pyro fires, update physics accordingly */
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1) {
                res.p1_fires++;
                if (res.p1_fires == 1) {
                    res.pyro1_fired = true;
                    res.pyro1_alt_m = ps.alt_m;
                    res.p1_fire_ms = t;
                    if (o.p1_opens_on_fire)
                        mock_pyro.p1_open = true;
                } else if (res.p1_refire_ms == 0) {
                    res.p1_refire_ms = t;
                }
                if (o.drogue_works)
                    ps.drogue_deployed = true;
            }
            if (ch == 2) {
                res.p2_fires++;
                if (res.p2_fires == 1) {
                    res.pyro2_fired = true;
                    res.pyro2_alt_m = ps.alt_m;
                    res.p2_fire_ms = t;
                }
                if (o.main_works)
                    ps.main_deployed = true;
            }
            prev_fires = mock_pyro.fire_count;
        }

        /* Physics: pad dwell, then ignition */
        if (t >= PAD_DWELL_MS) {
            float flight_t = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, flight_t, r);
        }
        if (ps.alt_m > res.apogee_m) {
            res.apogee_m = ps.alt_m;
            res.apogee_true_ms = t;
        }
        if (fabsf(ps.vel_ms) > res.max_speed_ms)
            res.max_speed_ms = fabsf(ps.vel_ms);

        /* Feed pressure sensor to firmware */
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);

        if (ctx.current_state == ASCENT && !res.reached_ascent) {
            res.reached_ascent = true;
            res.launch_ms = t;
            res.launch_alt_m = ps.alt_m;
        }
        if (ctx.current_state == FALLING && !res.reached_descent) {
            res.reached_descent = true;
            res.apogee_ms = t;
            res.apogee_alt_m = ps.alt_m;
        }
        if (ctx.current_state == DROGUE_DESCENT)
            res.saw_drogue_phase = true;
        if (ctx.current_state == CHUTE_DESCENT)
            res.saw_chute_phase = true;
        if (ctx.current_state == LANDED && !res.reached_landed) {
            res.reached_landed = true;
            res.flight_time_ms = t - ctx.launch_time;
            res.landed_ms = t;
            res.landing_speed_ms = fabsf(ps.vel_ms);
            break;
        }

        flight_update_outputs(&ctx, t);
    }
    res.main_forced = ctx.main_forced;

    res.sample_count = ctx.buf_count;
    char *cp = mock_uart_buf;
    while ((cp = strstr(cp, "$PYRO,")) != NULL) {
        res.telemetry_count++;
        cp++;
    }
    return res;
}

/* The nominal airframe: both canopies open, and a fired igniter goes open
 * circuit, which is what a lit charge does. */
static sim_result_t run_sim(config_t cfg, const rocket_profile_t *r, bool enable_pyros) {
    sim_opts_t o = {.enable_pyros = enable_pyros, .drogue_works = true, .main_works = true, .p1_opens_on_fire = true};
    return run_sim_opts(cfg, r, o);
}

/* A configured altitude trigger, in metres. */
static float trigger_m(uint16_t value, uint8_t units) {
    return units == 2 ? (float)value * 0.3048f : units == 1 ? (float)value : (float)value / 100.0f;
}

/* ── Configs ──────────────────────────────────────────────────────── */

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

/* ── Assertions ───────────────────────────────────────────────────── */

/* How far from its configured altitude an AGL channel may fire. The sensor
 * samples every 20 ms, and a ballistic rocket covers two metres of that. */
#define AGL_TOL_M 8.0f

static void assert_flight(const sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: no ASCENT", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_ascent, m);
    snprintf(m, sizeof(m), "%s: no FALLING/DROGUE/CHUTE", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_descent, m);
    snprintf(m, sizeof(m), "%s: no LANDED", l);
    TEST_ASSERT_TRUE_MESSAGE(r->reached_landed, m);
}
static void assert_p1(const sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: P1 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro1_fired, m);
}
static void assert_p2(const sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: P2 didn't fire", l);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro2_fired, m);
}
static void assert_order(const sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: main higher than drogue (P1=%.0f P2=%.0f)", l, r->pyro1_alt_m, r->pyro2_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(r->pyro1_alt_m >= r->pyro2_alt_m, m);
}
static void assert_data(const sim_result_t *r, const char *l) {
    char m[128];
    snprintf(m, sizeof(m), "%s: samples=%d", l, r->sample_count);
    TEST_ASSERT_TRUE_MESSAGE(r->sample_count > 10, m);
    snprintf(m, sizeof(m), "%s: telemetry=%d", l, r->telemetry_count);
    TEST_ASSERT_TRUE_MESSAGE(r->telemetry_count > 2, m);
}

/* ── Config suite runner ──────────────────────────────────────────── */

typedef config_t (*cfg_fn)(void);

/*
 * Run one pyro-config mode against every loaded rocket profile.
 *
 * Pyro thresholds that exceed the rocket's expected apogee are scaled down
 * proportionally so that every trigger mode is exercisable on small rockets.
 */
static void run_suite(cfg_fn make, const char *suite_name) {
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, g_num_rockets, "No rocket profiles loaded — check test_data/rockets.json");

    for (int i = 0; i < g_num_rockets; i++) {
        const rocket_profile_t *r = &g_rockets[i];
        config_t c = make();

        /* Scale AGL / FALLEN thresholds that would exceed the apogee */
        float apogee_ft = r->expected_apogee_m * 3.281f;
        if (c.pyro1_mode == PYRO_MODE_AGL && c.pyro1_value > (uint16_t)(apogee_ft * 0.9f))
            c.pyro1_value = (uint16_t)(apogee_ft * 0.5f);
        if (c.pyro2_mode == PYRO_MODE_AGL && c.pyro2_value > (uint16_t)(apogee_ft * 0.9f))
            c.pyro2_value = (uint16_t)(apogee_ft * 0.5f);
        if (c.pyro1_mode == PYRO_MODE_FALLEN && c.pyro1_value > (uint16_t)(apogee_ft * 0.5f))
            c.pyro1_value = (uint16_t)(apogee_ft * 0.3f);
        if (c.pyro2_mode == PYRO_MODE_FALLEN && c.pyro2_value > (uint16_t)(apogee_ft * 0.5f))
            c.pyro2_value = (uint16_t)(apogee_ft * 0.3f);

        char label[72];
        snprintf(label, sizeof(label), "%s@%s", suite_name, r->motor);

        sim_result_t res = run_sim(c, r, true);
        print_summary(label, &res);

        assert_flight(&res, label);
        assert_p1(&res, label);
        assert_data(&res, label);

        /* Main chute required for rockets that fly above ~100 m */
        if (r->expected_apogee_m >= 100.0f) {
            assert_p2(&res, label);
            assert_order(&res, label);
        }

        /* [REV-01] Every canopy in this runner works, so nothing may be
         * overridden: each channel goes out on its own trigger. */
        char m[160];
        snprintf(m, sizeof(m), "%s: the ladder forced the main on a flight whose drogue worked", label);
        TEST_ASSERT_FALSE_MESSAGE(res.main_forced, m);

        /* [PYR-MODE-02, REV-05] An AGL trigger fires at its altitude, not a
         * filter lag later. Only where the trigger is below apogee: above it
         * the channel fires at apogee by design (PYR-DEPLOY-01). */
        if (c.pyro1_mode == PYRO_MODE_AGL && trigger_m(c.pyro1_value, c.units) < res.apogee_m - 5.0f) {
            float want = trigger_m(c.pyro1_value, c.units);
            snprintf(m, sizeof(m), "%s: P1 AGL %.0f m fired at %.0f m", label, (double)want, (double)res.pyro1_alt_m);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(AGL_TOL_M, want, res.pyro1_alt_m, m);
        }
        if (c.pyro2_mode == PYRO_MODE_AGL && trigger_m(c.pyro2_value, c.units) < res.apogee_m - 5.0f &&
            res.pyro2_fired) {
            float want = trigger_m(c.pyro2_value, c.units);
            /* When both channels share a trigger the main waits out the
             * drogue's pulse (PYR-DEPLOY-02) and fires a step later. */
            snprintf(m, sizeof(m), "%s: P2 AGL %.0f m fired at %.0f m", label, (double)want, (double)res.pyro2_alt_m);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(AGL_TOL_M, want, res.pyro2_alt_m, m);
        }
    }
}

/* ── Test functions ───────────────────────────────────────────────── */

void setUp(void) { pp_init(); }
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

/* [TST-06] Chute effect: with chutes flight is longer than ballistic */
void test_TST_06_chute_effect(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need at least 4 rockets for chute_effect test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];
    config_t cfg = cfg_delay_agl();

    sim_result_t with = run_sim(cfg, r, true);
    sim_result_t without = run_sim(cfg, r, false);
    print_summary("chute: with", &with);
    print_summary("chute: without", &without);

    char msg[128];
    snprintf(msg, sizeof(msg), "With chutes (%ums) should be longer than ballistic (%ums)", with.flight_time_ms,
             without.flight_time_ms);
    TEST_ASSERT_TRUE_MESSAGE(with.flight_time_ms > without.flight_time_ms, msg);
}

/* [PYR-SAFE-01] No fire without continuity: pyro1 open → must not fire */
void test_PYR_SAFE_01_no_fire_without_continuity(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for safety test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];
    config_t cfg = cfg_delay_agl();

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
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    sim_result_t res = {0};
    uint8_t prev_fires = 0;
    uint32_t max_ms = (r->expected_apogee_m < 100.0f) ? 120000u : 600000u;

    for (uint32_t t = 0; t <= max_ms; t++) {
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
            physics_step(&ps, ft, r);
        }
        if (ps.alt_m > res.apogee_m)
            res.apogee_m = ps.alt_m;
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);
        if (ctx.current_state == LANDED)
            break;
    }

    print_summary("NoCont P1", &res);
    TEST_ASSERT_FALSE_MESSAGE(res.pyro1_fired, "Pyro1 fired despite no continuity");
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "Pyro2 should fire with good continuity");
}

/* [PYR-DEPLOY-01] A low flight puts both out on one event.
 *
 * This was test_PYR_SAFE_02_no_simultaneous_fire, which asserted the two fire
 * times DIFFER. That forbade exactly the case this exists to allow -- and it
 * was proving almost nothing anyway, because run_sim() clears
 * mock_pyro.firing before every step, so the 500 ms hardware separation it
 * was measuring is 1 ms here.
 *
 * What matters now: with both channels set to fire at apogee, both actually
 * deploy, and close enough together to be one event rather than two.
 *
 * The gap this measures is the SIMULATION's, not the hardware's. run_sim()
 * clears mock_pyro.firing each step, so the separation here is one step; on a
 * board it is FIRE_DURATION_MS, about 500 ms, because the two channels share a
 * common element and must not be energised together (PYR-DEPLOY-02). The
 * bound below is generous enough to cover both. */
void test_PYR_DEPLOY_01_low_flight_fires_both(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for the both-on-one-event test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];
    config_t cfg = (config_t){.id = "SS",
                              .name = "LowFly",
                              .pyro1_mode = PYRO_MODE_DELAY,
                              .pyro1_value = 0,
                              .pyro2_mode = PYRO_MODE_DELAY,
                              .pyro2_value = 0,
                              .units = 2};

    sim_result_t res = run_sim(cfg, r, true);
    print_summary("BothOnOneEvent", &res);

    assert_flight(&res, "BothOnOneEvent");
    assert_p1(&res, "BothOnOneEvent");
    assert_p2(&res, "BothOnOneEvent");

    char msg[160];
    uint32_t gap = (res.p1_fire_ms > res.p2_fire_ms) ? res.p1_fire_ms - res.p2_fire_ms
                                                     : res.p2_fire_ms - res.p1_fire_ms;
    snprintf(msg, sizeof(msg), "both should deploy on one event: P1=%u P2=%u gap=%u ms", res.p1_fire_ms,
             res.p2_fire_ms, gap);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro1_fired && res.pyro2_fired, msg);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(1000, gap, msg);
}

/* [SYS-DEPLOY-03] No firing during ascent */
void test_SYS_DEPLOY_03_no_fire_during_ascent(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for ascent-fire test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];
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
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    bool pyro_during_ascent = false;
    uint32_t max_ms = (r->expected_apogee_m < 100.0f) ? 120000u : 600000u;

    for (uint32_t t = 0; t <= max_ms; t++) {
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, r);
        }
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);

        if (ctx.current_state == ASCENT && mock_pyro.fire_count > 0)
            pyro_during_ascent = true;
        if (ctx.current_state == LANDED)
            break;
    }

    TEST_ASSERT_FALSE_MESSAGE(pyro_during_ascent, "Pyro fired during ASCENT — must only fire after apogee");
}

/* [PYR-FAULT-02] Overcurrent fault detection via FLAG pin */
void test_PYR_FAULT_02_overcurrent_detection(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for fault test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];
    config_t cfg = cfg_delay_delay(); /* fires quickly at apogee */

    mock_reset_all();
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    mock_pyro.fault = true;
    mock_uart_len = 0;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    ctx.config = cfg;
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    uint8_t prev_fires = 0;
    uint32_t max_ms = (r->expected_apogee_m < 100.0f) ? 120000u : 600000u;

    for (uint32_t t = 0; t <= max_ms; t++) {
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
            physics_step(&ps, ft, r);
        }
        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);

        if (ctx.pyro1_fault && ctx.pyro2_fault)
            break;
        if (ctx.current_state == LANDED)
            break;
    }

    TEST_ASSERT_TRUE_MESSAGE(ctx.pyro1_fault, "Pyro1 fault not detected");
    TEST_ASSERT_TRUE_MESSAGE(ctx.pyro2_fault, "Pyro2 fault not detected");

    hal_log_stop();
    flight_save_csv(&ctx);
    char buf[4096];
    int n = hal_fs_read_file("flight.csv", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "No CSV data");
    buf[n] = '\0';
}

/* [TST-05] All four rocket profiles reach sensible apogees */
void test_TST_05_rocket_profiles(void) {
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, g_num_rockets, "No rockets loaded");

    for (int i = 0; i < g_num_rockets; i++) {
        const rocket_profile_t *r = &g_rockets[i];
        config_t cfg = cfg_delay_delay();
        sim_result_t res = run_sim(cfg, r, true);

        char label[72];
        snprintf(label, sizeof(label), "profile@%s", r->motor);
        print_summary(label, &res);

        assert_flight(&res, label);

        /* Apogee within ±tolerance of expected (very loose — physics is approximate) */
        float lo = r->expected_apogee_m * (1.0f - r->apogee_tol);
        float hi = r->expected_apogee_m * (1.0f + r->apogee_tol);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: apogee %.0fm not in [%.0f, %.0f]m", label, res.apogee_m, lo, hi);
        TEST_ASSERT_TRUE_MESSAGE(res.apogee_m >= lo && res.apogee_m <= hi, msg);
    }
}

/* ── XIP stall robustness test ────────────────────────────────────── */
void test_XIP_stall_pyro_timing(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for XIP stall test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];

    mock_reset_all();
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
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    uint32_t max_ms = 600000u;
    uint8_t prev_fires = 0;
    bool p1_fired = false, p2_fired = false;
    float p1_alt = 0, p2_alt = 0;
    uint32_t stall_during_descent = 0;
    uint32_t phys_t = 0;

    for (uint32_t t = 0; t <= max_ms; t++) {
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

        if (t >= PAD_DWELL_MS) {
            uint32_t phys_start = (phys_t >= PAD_DWELL_MS) ? phys_t : PAD_DWELL_MS;
            uint32_t steps_needed = t - phys_start;
            for (uint32_t s = 0; s < steps_needed; s++) {
                float ft = (float)(phys_start + s - PAD_DWELL_MS) / 1000.0f;
                physics_step(&ps, ft, r);
            }
        }
        phys_t = t;

        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;

        uint32_t stall_before = mock_xip_total_stall_ms;
        ctx.current_state = step(&ctx, mock_time_ms);

        if (mock_time_ms > t)
            t = mock_time_ms;

        if (ctx.current_state == FALLING ||
            ctx.current_state == DROGUE_DESCENT ||
            ctx.current_state == CHUTE_DESCENT)
            stall_during_descent += (mock_xip_total_stall_ms - stall_before);

        if (ctx.current_state == LANDED)
            break;
    }

    printf("  XIP stall: %ums total, %d events, per_op=%ums\n", mock_xip_total_stall_ms, mock_xip_stall_count,
           mock_xip_stall_ms);
    printf("  XIP descent stall: %ums\n", stall_during_descent);
    printf("  XIP result: P1=%s@%.0fm  P2=%s@%.0fm  landed=%s\n", p1_fired ? "FIRED" : "MISSED", p1_alt,
           p2_fired ? "FIRED" : "MISSED", p2_alt, ctx.current_state == LANDED ? "yes" : "no");

    TEST_ASSERT_TRUE_MESSAGE(p1_fired, "Pyro1 failed to fire with XIP stalls");
    TEST_ASSERT_TRUE_MESSAGE(p2_fired, "Pyro2 failed to fire with XIP stalls");
    TEST_ASSERT_EQUAL_MESSAGE(LANDED, ctx.current_state, "Did not reach LANDED with XIP stalls");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, mock_xip_stall_count, "No XIP stalls occurred");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, mock_xip_total_stall_ms, "No XIP stall time accumulated");
}

/* [PYR-REFIRE-01] Re-fire when ballistic descent detected */
void test_PYR_REFIRE_01_refire_ballistic(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket for refire test");
    const rocket_profile_t *r = &g_rockets[ROCKET_IDX_L1];

    mock_reset_all();
    mock_pyro.p1_good = true;
    mock_pyro.p2_good = true;
    mock_pyro.p1_adc = 50;
    mock_pyro.p2_adc = 50;
    buzzer_stop_count = 0;
    buzzer_altitude_count = 0;
    buzzer_active_flag = true;

    flight_context_t ctx = {0};
    config_set_defaults(&ctx.config);
    ctx.config.pyro1_mode = PYRO_MODE_FALLEN;
    ctx.config.pyro1_value = 100; /* 100 m below apogee — fast ballistic descent */
    ctx.config.pyro2_mode = PYRO_MODE_DELAY;
    ctx.config.pyro2_value = 60; /* 60 s delay — won't fire in test window */
    ctx.config.units = 1;        /* meters */
    ctx.current_state = PAD_IDLE;
    ctx.ground_pressure = (int32_t)GROUND_PA;
    pp_test_prime((int32_t)GROUND_PA);

    physics_state_t ps = {0};
    uint8_t prev_fires = 0;
    bool initial_fired = false, refire_detected = false;
    uint32_t first_fire_time = 0, second_fire_time = 0;

    for (uint32_t t = 0; t <= 600000u; t++) {
        if (mock_pyro.fire_count > prev_fires) {
            uint8_t ch = mock_pyro.last_fire_channel;
            if (ch == 1) {
                if (!initial_fired) {
                    initial_fired = true;
                    first_fire_time = t;
                } else if (!refire_detected) {
                    refire_detected = true;
                    second_fire_time = t;
                }
            }
            prev_fires = mock_pyro.fire_count;
        }

        /* Failed deployment: drogue never deploys → rocket stays ballistic */
        if (t >= PAD_DWELL_MS) {
            float ft = (float)(t - PAD_DWELL_MS) / 1000.0f;
            physics_step(&ps, ft, r);
        }

        mock_time_ms = t;
        mock_pressure.pressure_pa = alt_m_to_pa(ps.alt_m);
        mock_pyro.firing = false;
        ctx.current_state = step(&ctx, t);

        if (refire_detected)
            break;
        if (ctx.current_state == LANDED)
            break;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Pyro1 never fired (FALLEN %dm, apogee~%.0fm)", ctx.config.pyro1_value,
             g_rockets[ROCKET_IDX_L1].expected_apogee_m);
    TEST_ASSERT_TRUE_MESSAGE(initial_fired, msg);

    snprintf(msg, sizeof(msg), "Re-fire not detected: initial=%ums  speed=%d cm/s (need < -3000)", first_fire_time,
             ctx.vertical_speed_cms);
    TEST_ASSERT_TRUE_MESSAGE(refire_detected, msg);

    /* The retry is now the first rung of the emergency ladder rather than the
     * old free-standing check_refire() window. It lands one grace period after
     * the initial fire, deterministically, instead of inside a 1-1.5 s window
     * that the descent rate happened to fall through. */
    uint32_t window_ms = second_fire_time - first_fire_time;
    snprintf(msg, sizeof(msg), "Re-fire window %ums not at the 2000 ms grace", window_ms);
    TEST_ASSERT_TRUE_MESSAGE(window_ms >= 1900 && window_ms <= 2300, msg);
}


/* ── Items 7, 8, 9: descent phase, emergency deploy, mach gate ─────── */

/* [PYR-REFIRE-02] A channel that opened fired its charge. Re-firing it cannot
 * help, so the ladder must skip straight to the main. */
void test_PYR_REFIRE_02_no_retry_when_opened(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_delay_agl();
    cfg.pyro2_value = 20; /* 20 m AGL: low enough that only the ladder beats it */
    sim_opts_t o = {.enable_pyros = true, .drogue_works = false, .main_works = true, .p1_opens_on_fire = true};
    sim_result_t res = run_sim_opts(cfg, &g_rockets[ROCKET_IDX_L1], o);
    print_summary("OpenedNoRetry", &res);

    char m[160];
    snprintf(m, sizeof(m), "drogue was commanded %d times; an opened channel must not be retried", res.p1_fires);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, res.p1_fires, m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "the main must go out once the drogue is spent");
    snprintf(m, sizeof(m), "main fired at %.0f m, below its 20 m trigger -- the ladder did not act",
             (double)res.pyro2_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_alt_m > 50.0f, m);
}

/* [FLT-EMRG-01] Drogue commanded, canopy never opens: the rocket keeps
 * accelerating, so the main must be brought forward over its own trigger. */
void test_FLT_EMRG_01_shredded_drogue_fires_main(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_delay_agl();
    cfg.pyro2_value = 20;
    sim_opts_t o = {.enable_pyros = true, .drogue_works = false, .main_works = true, .p1_opens_on_fire = false};
    sim_result_t res = run_sim_opts(cfg, &g_rockets[ROCKET_IDX_L1], o);
    print_summary("ShreddedDrogue", &res);

    char m[160];
    TEST_ASSERT_TRUE_MESSAGE(res.pyro1_fired, "drogue should have been commanded at apogee");
    snprintf(m, sizeof(m), "the charge never lit, so exactly one retry was due; got %d fires", res.p1_fires);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, res.p1_fires, m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "the main must go out after the retry is spent");
    snprintf(m, sizeof(m), "main fired at %.0f m; its configured trigger was 20 m, so it was NOT brought forward",
             (double)res.pyro2_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_alt_m > 50.0f, m);
    TEST_ASSERT_TRUE_MESSAGE(res.reached_landed, "a failed drogue must still reach LANDED");
}

/* [FLT-EMRG-02] The ladder must not act on speed alone. A drogue set below
 * apogee means a deliberate free fall down to it, and free fall is fast; a
 * bare descent-rate trigger would fire the main over the top of that plan. */
void test_FLT_EMRG_02_freefall_to_trigger_not_overridden(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_agl_agl(); /* drogue 400 m, main 200 m */
    sim_result_t res = run_sim(cfg, &g_rockets[ROCKET_IDX_L1], true);
    print_summary("FreefallToTrigger", &res);

    char m[176];
    TEST_ASSERT_TRUE_MESSAGE(res.pyro1_fired, "drogue must fire at its 400 m trigger");
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "main must fire at its 200 m trigger");
    snprintf(m, sizeof(m), "main fired at %.0f m before the drogue at %.0f m: a rate trigger pre-empted the plan",
             (double)res.pyro2_alt_m, (double)res.pyro1_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(res.p2_fire_ms > res.p1_fire_ms, m);
    /* The real question is whether the ladder pre-empted the plan. Had it
     * done so the main would have gone out near apogee, as it does in
     * test_FLT_EMRG_01 (1098 m). Staying down at its own trigger is the
     * proof that free fall alone did not trigger anything. */
    snprintf(m, sizeof(m), "main fired at %.0f m off a %.0f m apogee: the ladder pre-empted its trigger",
             (double)res.pyro2_alt_m, (double)res.apogee_m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_alt_m < 300.0f, m);
}

/* [FLT-MACH-02] A fast subsonic flight is never locked: past 100 ft/s but far
 * from the flag, it finds apogee as a plain altimeter would. */
void test_FLT_MACH_02_fast_subsonic_flight_not_locked(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    sim_result_t res = run_sim(cfg_delay_agl(), &g_rockets[ROCKET_IDX_L1], true);
    print_summary("MachGate", &res);

    char m[176];
    snprintf(m, sizeof(m), "peak speed %.0f m/s is not a fast flight, so this proves nothing", (double)res.max_speed_ms);
    TEST_ASSERT_TRUE_MESSAGE(res.max_speed_ms > 30.5f, m);
    TEST_ASSERT_TRUE_MESSAGE(res.reached_descent, "apogee was never declared");

    int32_t lag_ms = (int32_t)res.apogee_ms - (int32_t)res.apogee_true_ms;
    snprintf(m, sizeof(m), "apogee declared %d ms after the real one (true=%u firmware=%u)", lag_ms,
             res.apogee_true_ms, res.apogee_ms);
    TEST_ASSERT_TRUE_MESSAGE(lag_ms >= 0 && lag_ms < 1000, m);
}

/* [FLT-DESC-01] Phase comes from the rate. With no drogue configured at all,
 * the old machine sat in FALLING for the rest of the flight because its only
 * exit was pyro1_fired. The main opens, the rate steadies, and the phase must
 * follow the rocket. */
void test_FLT_DESC_01_phase_without_pyros(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_agl_agl();
    cfg.pyro1_mode = PYRO_MODE_NONE; /* single deploy: nothing on channel 1 */
    cfg.pyro1_value = 0;
    cfg.units = 1;         /* metres: cfg_agl_agl is in feet */
    cfg.pyro2_value = 400; /* high enough that a descent phase can establish */
    sim_result_t res = run_sim(cfg, &g_rockets[ROCKET_IDX_L1], true);
    print_summary("SingleDeploy", &res);

    char m[176];
    TEST_ASSERT_FALSE_MESSAGE(res.pyro1_fired, "channel 1 is NONE and must never fire");
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, "the main must still fire at its own trigger");
    snprintf(m, sizeof(m), "never reached a canopy phase though the main opened at %.0f m",
             (double)res.pyro2_alt_m);
    TEST_ASSERT_TRUE_MESSAGE(res.saw_chute_phase || res.saw_drogue_phase, m);
    TEST_ASSERT_TRUE_MESSAGE(res.reached_landed, "a single-deploy flight must reach LANDED");
}

/* [FLT-DESC-02] The deadlock this pass exists to kill. No continuity on
 * either channel, so nothing deploys and the rocket comes in ballistic. It
 * must still reach LANDED, because that is what calls hal_log_stop() and the
 * log of a flight that failed is the log that matters. */
void test_FLT_DESC_02_ballistic_reaches_landed(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    sim_result_t res = run_sim(cfg_delay_agl(), &g_rockets[ROCKET_IDX_L1], false);
    print_summary("BallisticNoPyros", &res);

    TEST_ASSERT_FALSE_MESSAGE(res.pyro1_fired, "no continuity: nothing may fire");
    TEST_ASSERT_FALSE_MESSAGE(res.pyro2_fired, "no continuity: nothing may fire");
    TEST_ASSERT_TRUE_MESSAGE(res.reached_descent, "must still detect apogee");
    TEST_ASSERT_TRUE_MESSAGE(res.reached_landed, "ballistic flight never reached LANDED: the log is lost");
}


/* ── Code review 2026-09-24 ───────────────────────────────────────── */

/* [FLT-EMRG-01, PYR-MODE-02, REV-01] The regression guard the review asked
 * for: a drogue that opens, and a main set to 500 ft AGL. The main must go out
 * at 500 ft, not two seconds after the drogue. */
void test_REV01_working_drogue_main_at_its_trigger(void) {
    for (int i = 0; i < g_num_rockets; i++) {
        const rocket_profile_t *r = &g_rockets[i];
        config_t cfg = cfg_delay_agl();
        cfg.pyro2_value = 500; /* ft */
        sim_opts_t o = {.enable_pyros = true, .drogue_works = true, .main_works = true, .p1_opens_on_fire = true};
        sim_result_t res = run_sim_opts(cfg, r, o);
        char label[64];
        snprintf(label, sizeof(label), "Main500@%s", r->motor);
        print_summary(label, &res);

        char m[176];
        snprintf(m, sizeof(m), "%s: main forced at %.0f m, %u ms after the drogue", label, (double)res.pyro2_alt_m,
                 res.p2_fire_ms - res.p1_fire_ms);
        TEST_ASSERT_FALSE_MESSAGE(res.main_forced, m);
        if (res.apogee_m > 152.4f + 10.0f) {
            TEST_ASSERT_TRUE_MESSAGE(res.pyro2_fired, label);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(AGL_TOL_M, 152.4f, res.pyro2_alt_m, m);
        }
    }
}

/* [PYR-MODE-02, REV-05] A drogue set to 400 ft AGL on a ballistic descent.
 * The filtered altitude trails the rocket by rate x tau, which at these
 * speeds was 56 m. */
void test_REV05_agl_drogue_fires_at_its_altitude(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    sim_result_t res = run_sim(cfg_agl_agl(), &g_rockets[ROCKET_IDX_L1], true);
    print_summary("AglLag", &res);
    char m[160];
    snprintf(m, sizeof(m), "400 ft (122 m) drogue fired at %.0f m, descending at up to %.0f m/s",
             (double)res.pyro1_alt_m, (double)res.max_speed_ms);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(AGL_TOL_M, 121.9f, res.pyro1_alt_m, m);
}

/* [FLT-EMRG-01, REV-01] The failure case, told the way the pad narrative
 * tells it: the charge lit, the drogue did not deploy, the rocket keeps
 * accelerating. The main must be brought forward on that evidence -- well
 * above its own trigger, and within a few seconds of the drogue. */
void test_REV01_failed_drogue_brings_the_main_forward(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_delay_agl();
    cfg.pyro2_value = 500; /* ft */
    sim_opts_t o = {.enable_pyros = true, .drogue_works = false, .main_works = true, .p1_opens_on_fire = true};
    sim_result_t res = run_sim_opts(cfg, &g_rockets[ROCKET_IDX_L1], o);
    print_summary("FailedDrogue", &res);

    char m[176];
    TEST_ASSERT_TRUE_MESSAGE(res.main_forced, "the ladder never acted on a drogue that failed");
    uint32_t after = res.p2_fire_ms - res.p1_fire_ms;
    snprintf(m, sizeof(m), "main forced %u ms after the drogue at %.0f m", after, (double)res.pyro2_alt_m);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(7000, after, m);
    TEST_ASSERT_TRUE_MESSAGE(res.pyro2_alt_m > 152.4f + 300.0f, m);
    TEST_ASSERT_TRUE_MESSAGE(res.reached_landed, "a failed drogue must still reach LANDED");
}

/* [FLT-EMRG-01, DAT-04, REV-16] An emergency deployment is recorded as one.
 * After the shredded-drogue flight the log is the only way to tell a forced
 * main from a configured one. */
void test_REV16_forced_main_is_in_the_log(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_num_rockets > ROCKET_IDX_L1, "Need L1 rocket");
    config_t cfg = cfg_delay_agl();
    cfg.pyro2_value = 20;
    sim_opts_t o = {.enable_pyros = true, .drogue_works = false, .main_works = true, .p1_opens_on_fire = true};
    sim_result_t res = run_sim_opts(cfg, &g_rockets[ROCKET_IDX_L1], o);
    TEST_ASSERT_TRUE(res.main_forced);

    static char log[65536];
    int n = hal_fs_read_file("flight_log.csv", log, (int)sizeof(log) - 1);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "no flight log");
    log[n] = '\0';
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "MAIN_FORCED"), "the log does not say the main was forced");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== Loading rocket profiles ===\n");
    int n = load_rockets_json("test_data/rockets.json");
    if (n <= 0) {
        printf("FATAL: could not load test_data/rockets.json — aborting\n");
        return 1;
    }
    printf("Loaded %d rocket profile%s\n\n", n, n == 1 ? "" : "s");

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
    RUN_TEST(test_PYR_DEPLOY_01_low_flight_fires_both);
    RUN_TEST(test_SYS_DEPLOY_03_no_fire_during_ascent);
    RUN_TEST(test_PYR_FAULT_02_overcurrent_detection);
    RUN_TEST(test_TST_05_rocket_profiles);
    RUN_TEST(test_XIP_stall_pyro_timing);
    RUN_TEST(test_PYR_REFIRE_01_refire_ballistic);
    RUN_TEST(test_PYR_REFIRE_02_no_retry_when_opened);
    RUN_TEST(test_FLT_EMRG_01_shredded_drogue_fires_main);
    RUN_TEST(test_FLT_EMRG_02_freefall_to_trigger_not_overridden);
    RUN_TEST(test_FLT_MACH_02_fast_subsonic_flight_not_locked);
    RUN_TEST(test_FLT_DESC_01_phase_without_pyros);
    RUN_TEST(test_FLT_DESC_02_ballistic_reaches_landed);

    /* Code review 2026-09-24 */
    RUN_TEST(test_REV01_working_drogue_main_at_its_trigger);
    RUN_TEST(test_REV05_agl_drogue_fires_at_its_altitude);
    RUN_TEST(test_REV01_failed_drogue_brings_the_main_forward);
    RUN_TEST(test_REV16_forced_main_is_in_the_log);
    return UNITY_END();
}
