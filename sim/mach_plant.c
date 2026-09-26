/*
 * See mach_plant.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mach_plant.h"
#include <math.h>

/* ── Atmosphere ───────────────────────────────────────────────────── */

static float isa_pa_at_elev(float elev_m) {
    return 101325.0f * powf(1.0f - MP_LAPSE * elev_m / 288.15f, MP_G / (MP_R * MP_LAPSE));
}

float mp_pad_pa(const mp_site_t *s) {
    return isa_pa_at_elev(s->elev_m);
}

float mp_temp_k(const mp_site_t *s, float h_agl_m) {
    float t0 = s->temp_c + 273.15f;
    float z = s->elev_m + h_agl_m;
    float zt = z < MP_TROPOPAUSE_M ? z : MP_TROPOPAUSE_M;
    return t0 - MP_LAPSE * (zt - s->elev_m);
}

float mp_pressure_pa(const mp_site_t *s, float h_agl_m) {
    float t0 = s->temp_c + 273.15f;
    float p0 = mp_pad_pa(s);
    float z = s->elev_m + h_agl_m;
    const float n = MP_G / (MP_R * MP_LAPSE);
    if (z <= MP_TROPOPAUSE_M)
        return p0 * powf(mp_temp_k(s, h_agl_m) / t0, n);
    float tt = mp_temp_k(s, MP_TROPOPAUSE_M - s->elev_m);
    float pt = p0 * powf(tt / t0, n);
    return pt * expf(-MP_G * (z - MP_TROPOPAUSE_M) / (MP_R * tt));
}

float mp_sound_ms(const mp_site_t *s, float h_agl_m) {
    return sqrtf(MP_GAMMA * MP_R * mp_temp_k(s, h_agl_m));
}

/* ── The rocket ───────────────────────────────────────────────────── */

void mp_launch(mp_state_t *st) {
    st->t = 0.0f;
    st->h = 0.0f;
    st->v = 0.0f;
    st->mach = 0.0f;
    st->max_mach = 0.0f;
    st->canopy = false;
    st->apogee = false;
    st->apogee_t = 0.0f;
    st->apogee_h = 0.0f;
    st->landed = false;
}

/* The wave-drag rise: a bump centred just past Mach 1. */
static float drag_factor(const mp_rocket_t *r, float mach) {
    float m = fabsf(mach);
    float d = (m - 1.05f) / 0.25f;
    return 1.0f + r->wave_rise * expf(-d * d);
}

void mp_step(mp_state_t *st, const mp_site_t *s, const mp_rocket_t *r, float dt) {
    if (st->landed)
        return;
    float rho = mp_pressure_pa(s, st->h) / (MP_R * mp_temp_k(s, st->h));
    float a;
    if (st->canopy && st->v < 0.0f) {
        /* Toward the canopy's terminal rate, from wherever it opened. */
        float k = MP_G / (r->canopy_ms * r->canopy_ms);
        a = -MP_G + k * st->v * st->v;
    } else {
        float burning = st->t < r->burn_s ? 1.0f : 0.0f;
        float mass = r->dry_kg + r->prop_kg * (burning ? (1.0f - st->t / r->burn_s) : 0.0f);
        float drag = 0.5f * rho * st->v * fabsf(st->v) * r->cda_m2 * drag_factor(r, st->mach);
        a = (burning * r->thrust_n - drag) / mass - MP_G;
    }
    float v_before = st->v;
    st->v += a * dt;
    st->h += st->v * dt;
    st->t += dt;
    st->mach = st->v / mp_sound_ms(s, st->h);
    if (fabsf(st->mach) > st->max_mach)
        st->max_mach = fabsf(st->mach);
    if (!st->apogee && st->t > r->burn_s && v_before > 0.0f && st->v <= 0.0f) {
        st->apogee = true;
        st->apogee_t = st->t;
        st->apogee_h = st->h;
    }
    if (st->h <= 0.0f && st->t > r->burn_s) {
        st->h = 0.0f;
        st->v = 0.0f;
        st->mach = 0.0f;
        st->landed = true;
    }
}

/* ── What the ports and the bay add ───────────────────────────────── */

float mp_port_error_pa(const mp_port_t *e, float mach, float static_pa) {
    float m = fabsf(mach);
    if (m < 0.85f)
        return 0.0f;
    float c = m < 1.0f ? e->below * (m - 0.85f) / 0.15f : e->above + e->slope * (m - 1.0f);
    float q = 0.5f * MP_GAMMA * static_pa * m * m;
    return e->sign * c * q;
}

float mp_charge_pa(const mp_charge_t *c, float since_fire_s) {
    if (since_fire_s < 0.0f)
        return 0.0f;
    return c->peak_pa * expf(-since_fire_s / c->tau_s);
}
