/*
 * A flight the bench can fly. See flight_sim.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flight_sim.h"
#include "atmosphere.h"
#include <math.h>

#define G0 9.80665f
#define TOP_M 32000.0f /* the profile's ceiling: the top of the standard's third layer */

bool fsim_start(fsim_t *s, const fsim_params_t *p, float ground_pa) {
    if (p->boost_s <= 0.0f || p->apogee_m <= p->main_alt_m || p->main_alt_m < 0.0f || p->drogue_ms <= 0.0f ||
        p->main_ms <= 0.0f || p->pad_s < 0.0f || ground_pa <= 0.0f)
        return false;
    s->p = *p;
    s->pad_pa = ground_pa;
    s->pad_msl = atmos_altitude_m(ground_pa);
    if (s->pad_msl + p->apogee_m > TOP_M)
        return false;
    /* apogee = a tb^2 / 2 + (a tb)^2 / 2g, solved for a. */
    float tb = p->boost_s;
    s->accel = G0 * (-0.5f + sqrtf(0.25f + 2.0f * p->apogee_m / (G0 * tb * tb)));
    s->t_burn = tb;
    s->v_burn = s->accel * tb;
    s->h_burn = 0.5f * s->accel * tb * tb;
    s->t_apogee = tb + s->v_burn / G0;
    if (s->p.ballistic_ms <= 0.0f)
        s->p.ballistic_ms = FSIM_BALLISTIC_MS;
    s->t_desc = s->t_apogee;
    s->h_desc = p->apogee_m;
    s->v_desc = 0.0f;
    s->phase = FSIM_PAD;
    s->t_landed = 0.0f;
    return true;
}

#define DESCENT_STEP_S 0.02f

/* The rate the rocket settles at, at this height. */
static float terminal_rate(const fsim_t *s, float h) {
    float above_main = s->p.drogue_fails ? s->p.ballistic_ms : s->p.drogue_ms;
    if (h <= s->p.main_alt_m)
        return s->p.main_fails ? above_main : s->p.main_ms;
    if (!s->p.thin_air)
        return above_main;
    return above_main / atmos_pad_air_ratio(atmos_pressure_pa(s->pad_msl + h), s->pad_pa);
}

/* Drag as the square of the speed, in balance with gravity at the terminal
 * rate. A canopy opening on a fast rocket slows it no further than that. */
static float descent_speed_after(float v, float terminal, float dt) {
    float ratio = v / terminal;
    float next = v + G0 * (1.0f - ratio * ratio) * dt;
    return v > terminal && next < terminal ? terminal : next;
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
        s->v_desc = descent_speed_after(s->v_desc, terminal_rate(s, s->h_desc), dt);
        s->h_desc -= s->v_desc * dt;
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
    return atmos_pressure_pa(s->pad_msl + fsim_altitude(s, t));
}

fsim_phase_t fsim_phase(const fsim_t *s) {
    return s->phase;
}

const char *fsim_phase_name(fsim_phase_t ph) {
    static const char *const names[] = {"pad", "boost", "coast", "drogue", "main", "landed"};
    return (unsigned)ph < sizeof(names) / sizeof(names[0]) ? names[ph] : "?";
}
