/*
 * See physics.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "physics.h"
#include <math.h>

/* ── Atmosphere: U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562) ─── */

/* Table 4: each layer's base (geopotential m), base temperature and lapse
 * rate. Base pressures are not tabulated here: they follow from the
 * hydrostatic equation (eqs. 33a/33b), so the published pressures test it. */
typedef struct {
    double base_m, base_k, lapse_k_per_m;
} layer_t;

static const layer_t USSA76[] = {
    {0.0, 288.15, -0.0065}, {11000.0, 216.65, 0.0},     {20000.0, 216.65, 0.0010},  {32000.0, 228.65, 0.0028},
    {47000.0, 270.65, 0.0}, {51000.0, 270.65, -0.0028}, {71000.0, 214.65, -0.0020},
};
#define N_LAYERS (int)(sizeof(USSA76) / sizeof(USSA76[0]))
#define USSA76_TOP_M 84852.0

static const double HYDROSTATIC_K = (double)PHYS_G0 / PHYS_R_AIR; /* g0·M0/R*, 1/K·m⁻¹ */

static double layer_ratio(const layer_t *l, double dh) {
    if (l->lapse_k_per_m == 0.0)
        return exp(-HYDROSTATIC_K * dh / l->base_k);
    return pow(l->base_k / (l->base_k + l->lapse_k_per_m * dh), HYDROSTATIC_K / l->lapse_k_per_m);
}

static double base_pa[N_LAYERS];

static void integrate_base_pressures(void) {
    if (base_pa[0] != 0.0)
        return;
    base_pa[0] = PHYS_SEA_LEVEL_PA;
    for (int i = 1; i < N_LAYERS; i++)
        base_pa[i] = base_pa[i - 1] * layer_ratio(&USSA76[i - 1], USSA76[i].base_m - USSA76[i - 1].base_m);
}

static int layer_index(double h) {
    int i = N_LAYERS - 1;
    while (i > 0 && h < USSA76[i].base_m)
        i--;
    return i;
}

static double temperature_k(double h) {
    const layer_t *l = &USSA76[layer_index(h)];
    double top_k = l->base_k + l->lapse_k_per_m * (USSA76_TOP_M - l->base_m);
    if (h > USSA76_TOP_M)
        return top_k;
    return l->base_k + l->lapse_k_per_m * (h - l->base_m);
}

static double pressure_pa(double h) {
    integrate_base_pressures();
    int i = layer_index(h);
    if (h <= USSA76_TOP_M)
        return base_pa[i] * layer_ratio(&USSA76[i], h - USSA76[i].base_m);
    double top_pa = base_pa[i] * layer_ratio(&USSA76[i], USSA76_TOP_M - USSA76[i].base_m);
    return top_pa * exp(-HYDROSTATIC_K * (h - USSA76_TOP_M) / temperature_k(h));
}

float physics_pressure_pa(float alt_m) {
    return (float)pressure_pa(alt_m);
}

float physics_temperature_k(float alt_m) {
    return (float)temperature_k(alt_m);
}

float physics_density_kg_m3(float alt_m) {
    return (float)(pressure_pa(alt_m) / (PHYS_R_AIR * temperature_k(alt_m)));
}

/* ── The flight ───────────────────────────────────────────────────── */

/* Terminal speeds at sea level, g/k: drogue about 12 m/s, main about 2.5 m/s,
 * no canopy about 200 m/s. */
#define DROGUE_DAMPING_PER_S 0.8f
#define MAIN_DAMPING_PER_S 4.0f
#define BALLISTIC_DAMPING_PER_S 0.05f

#define BURN_SEARCH_MAX_S 200.0f
#define BURN_SEARCH_STEPS 50

/* Burn time whose vacuum boost and coast peak at target_m. 0 when the thrust
 * cannot lift the rocket at all. */
static float vacuum_burn_time_s(float thrust_accel_ms2, float target_m) {
    float a_net = thrust_accel_ms2 - PHYS_G0;
    if (a_net <= 0.0f)
        return 0.0f;
    float lo = 0.0f, hi = BURN_SEARCH_MAX_S;
    for (int i = 0; i < BURN_SEARCH_STEPS; i++) {
        float t = (lo + hi) / 2.0f;
        float v_burnout = a_net * t;
        float apogee = 0.5f * a_net * t * t + v_burnout * v_burnout / (2.0f * PHYS_G0);
        if (apogee < target_m)
            lo = t;
        else
            hi = t;
    }
    return (lo + hi) / 2.0f;
}

void physics_reset(physics_state_t *ps) {
    *ps = (physics_state_t){
        .drogue_damping_per_s = DROGUE_DAMPING_PER_S,
        .main_damping_per_s = MAIN_DAMPING_PER_S,
        .ballistic_damping_per_s = BALLISTIC_DAMPING_PER_S,
    };
}

void physics_init(physics_state_t *ps, float target_alt_m) {
    physics_reset(ps);
    if (target_alt_m > 50000.0f)
        ps->thrust_accel_ms2 = 5.0f * PHYS_G0;
    else if (target_alt_m > 500.0f)
        ps->thrust_accel_ms2 = 10.0f * PHYS_G0;
    else
        ps->thrust_accel_ms2 = 20.0f * PHYS_G0;
    ps->burn_time_s = vacuum_burn_time_s(ps->thrust_accel_ms2, target_alt_m);
}

void physics_set_profile(physics_state_t *ps, float thrust_accel_ms2, float burn_time_s) {
    ps->thrust_accel_ms2 = thrust_accel_ms2;
    ps->burn_time_s = burn_time_s;
}

void physics_set_damping(physics_state_t *ps, float drogue_per_s, float main_per_s, float ballistic_per_s) {
    ps->drogue_damping_per_s = drogue_per_s;
    ps->main_damping_per_s = main_per_s;
    ps->ballistic_damping_per_s = ballistic_per_s;
}

static float descent_damping_per_s(const physics_state_t *ps) {
    if (ps->main_deployed)
        return ps->main_damping_per_s;
    if (ps->drogue_deployed)
        return ps->drogue_damping_per_s;
    return ps->ballistic_damping_per_s;
}

void physics_step(physics_state_t *ps, float since_launch_s) {
    if (ps->on_ground)
        return;

    float a = -PHYS_G0;
    if (since_launch_s < ps->burn_time_s)
        a += ps->thrust_accel_ms2;
    if (ps->vel_ms < 0.0f) {
        float sigma = physics_density_kg_m3(ps->alt_m) / physics_density_kg_m3(0.0f);
        a += descent_damping_per_s(ps) * sigma * -ps->vel_ms;
    }

    ps->vel_ms += a * PHYS_STEP_S;
    ps->alt_m += ps->vel_ms * PHYS_STEP_S;

    if (ps->alt_m > ps->apogee_m)
        ps->apogee_m = ps->alt_m;

    if (ps->alt_m <= 0.0f) {
        ps->alt_m = 0;
        ps->vel_ms = 0;
        ps->on_ground = true;
    }
}

void physics_deploy_drogue(physics_state_t *ps) {
    ps->drogue_deployed = true;
}

void physics_deploy_main(physics_state_t *ps) {
    ps->main_deployed = true;
}

/* ── WASM flat API ────────────────────────────────────────────────── */

static physics_state_t g_physics;

void physics_wasm_init(float target_alt_m) {
    physics_init(&g_physics, target_alt_m);
}
void physics_wasm_reset(void) {
    physics_reset(&g_physics);
}
void physics_wasm_step(float since_launch_s) {
    physics_step(&g_physics, since_launch_s);
}
void physics_wasm_deploy_drogue(void) {
    physics_deploy_drogue(&g_physics);
}
void physics_wasm_deploy_main(void) {
    physics_deploy_main(&g_physics);
}
float physics_wasm_alt_m(void) {
    return g_physics.alt_m;
}
float physics_wasm_vel_ms(void) {
    return g_physics.vel_ms;
}
float physics_wasm_pressure_pa(void) {
    return physics_pressure_pa(g_physics.alt_m);
}
float physics_wasm_apogee_m(void) {
    return g_physics.apogee_m;
}
int physics_wasm_on_ground(void) {
    return g_physics.on_ground ? 1 : 0;
}
int physics_wasm_drogue_deployed(void) {
    return g_physics.drogue_deployed ? 1 : 0;
}
int physics_wasm_main_deployed(void) {
    return g_physics.main_deployed ? 1 : 0;
}
