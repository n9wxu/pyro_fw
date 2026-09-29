/*
 * A flight the bench can fly. See flight_sim.h.
 *
 * The atmosphere is the U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562):
 * its base values for the layers from 0 to 32 km, and its hydrostatic
 * equation, geopotential altitude throughout.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_sim.h"
#include <math.h>

#define G0 9.80665f
#define R_STAR 8.31432f /* J/(mol K), the standard's own value */
#define M_AIR 0.0289644f

typedef struct {
    float h, t, lapse, p;
} layer_t;

static const layer_t layers[] = {
    {0.0f, 288.15f, -0.0065f, 101325.0f},
    {11000.0f, 216.65f, 0.0f, 22632.06f},
    {20000.0f, 216.65f, 0.001f, 5474.889f},
};
#define N_LAYERS (int)(sizeof(layers) / sizeof(layers[0]))
#define TOP_M 32000.0f

static const layer_t *layer_at(float h) {
    int i = N_LAYERS - 1;
    while (i > 0 && h < layers[i].h)
        i--;
    return &layers[i];
}

static float temperature(float h) {
    const layer_t *l = layer_at(h);
    return l->t + l->lapse * (h - l->h);
}

float fsim_isa_pressure(float h) {
    if (h > TOP_M)
        h = TOP_M;
    const layer_t *l = layer_at(h);
    float dh = h - l->h;
    if (l->lapse == 0.0f)
        return l->p * expf(-G0 * M_AIR * dh / (R_STAR * l->t));
    return l->p * powf(l->t / (l->t + l->lapse * dh), G0 * M_AIR / (R_STAR * l->lapse));
}

float fsim_isa_altitude(float pa) {
    int i = N_LAYERS - 1;
    while (i > 0 && pa > layers[i].p)
        i--;
    const layer_t *l = &layers[i];
    if (l->lapse == 0.0f)
        return l->h - logf(pa / l->p) * R_STAR * l->t / (G0 * M_AIR);
    return l->h + (l->t / powf(pa / l->p, R_STAR * l->lapse / (G0 * M_AIR)) - l->t) / l->lapse;
}

float fsim_isa_density(float h) {
    return fsim_isa_pressure(h) * M_AIR / (R_STAR * temperature(h));
}

bool fsim_start(fsim_t *s, const fsim_params_t *p, float ground_pa) {
    if (p->boost_s <= 0.0f || p->apogee_m <= p->main_alt_m || p->main_alt_m < 0.0f || p->drogue_ms <= 0.0f ||
        p->main_ms <= 0.0f || p->pad_s < 0.0f)
        return false;
    s->p = *p;
    s->pad_msl = fsim_isa_altitude(ground_pa);
    if (s->pad_msl + p->apogee_m > TOP_M)
        return false;
    /* apogee = a tb^2 / 2 + (a tb)^2 / 2g, solved for a. */
    float tb = p->boost_s;
    s->accel = G0 * (-0.5f + sqrtf(0.25f + 2.0f * p->apogee_m / (G0 * tb * tb)));
    s->t_burn = tb;
    s->v_burn = s->accel * tb;
    s->h_burn = 0.5f * s->accel * tb * tb;
    s->t_apogee = tb + s->v_burn / G0;
    s->t_desc = s->t_apogee;
    s->h_desc = p->apogee_m;
    s->phase = FSIM_PAD;
    s->t_landed = 0.0f;
    return true;
}

#define DESCENT_STEP_S 0.02f

static float descent_rate(const fsim_t *s, float h) {
    if (h <= s->p.main_alt_m)
        return s->p.main_ms;
    if (!s->p.thin_air)
        return s->p.drogue_ms;
    return s->p.drogue_ms * sqrtf(fsim_isa_density(s->pad_msl) / fsim_isa_density(s->pad_msl + h));
}

float fsim_altitude(fsim_t *s, float t) {
    float tau = t - s->p.pad_s;
    if (tau < 0.0f) {
        s->phase = FSIM_PAD;
        return 0.0f;
    }
    if (tau < s->t_burn) {
        s->phase = FSIM_BOOST;
        return 0.5f * s->accel * tau * tau;
    }
    if (tau < s->t_apogee) {
        float c = tau - s->t_burn;
        s->phase = FSIM_COAST;
        return s->h_burn + s->v_burn * c - 0.5f * G0 * c * c;
    }
    while (s->t_desc < tau && s->h_desc > 0.0f) {
        float dt = tau - s->t_desc < DESCENT_STEP_S ? tau - s->t_desc : DESCENT_STEP_S;
        s->h_desc -= descent_rate(s, s->h_desc) * dt;
        s->t_desc += dt;
        if (s->h_desc <= 0.0f) {
            s->h_desc = 0.0f;
            s->t_landed = s->t_desc;
        }
    }
    s->phase = s->h_desc <= 0.0f ? FSIM_LANDED : (s->h_desc <= s->p.main_alt_m ? FSIM_MAIN : FSIM_DROGUE);
    return s->h_desc;
}

float fsim_pressure(fsim_t *s, float t) {
    return fsim_isa_pressure(s->pad_msl + fsim_altitude(s, t));
}

fsim_phase_t fsim_phase(const fsim_t *s) {
    return s->phase;
}

const char *fsim_phase_name(fsim_phase_t ph) {
    static const char *const names[] = {"pad", "boost", "coast", "drogue", "main", "landed"};
    return (unsigned)ph < sizeof(names) / sizeof(names[0]) ? names[ph] : "?";
}
