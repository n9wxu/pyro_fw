/*
 * A plant for the Mach lockout's tests: a real atmosphere at any pad, a
 * rocket that can go supersonic, and the pressure its static ports report.
 *
 * physics.c drives the browser simulator and stays as it is. This one is for
 * the host tests, where the pad's temperature and elevation matter: every
 * threshold of the lockout is a pressure ratio whose Mach meaning depends on
 * the air's temperature (docs/mach-lockout-prompt.md).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MACH_PLANT_H
#define MACH_PLANT_H

#include <stdbool.h>

#define MP_G 9.80665f
#define MP_R 287.05f
#define MP_GAMMA 1.4f
#define MP_LAPSE 0.0065f         /* K/m, to the tropopause */
#define MP_TROPOPAUSE_M 11000.0f /* ASL */

/* The pad: its air temperature, and its elevation above sea level. Its
 * pressure is the standard atmosphere's at that elevation. */
typedef struct {
    float temp_c;
    float elev_m;
} mp_site_t;

float mp_pad_pa(const mp_site_t *s);
float mp_temp_k(const mp_site_t *s, float h_agl_m);
float mp_pressure_pa(const mp_site_t *s, float h_agl_m);
float mp_sound_ms(const mp_site_t *s, float h_agl_m);

/* A single-stage rocket flying straight up. Drag rises through Mach 1 and
 * falls back beyond it, as a real airframe's does. */
typedef struct {
    float dry_kg;
    float prop_kg;  /* burned at an even rate */
    float thrust_n; /* constant over the burn */
    float burn_s;
    float cda_m2;    /* drag area below the transonic rise */
    float wave_rise; /* extra drag at Mach 1.05, as a fraction of cda */
    float canopy_ms; /* terminal rate once a canopy is out */
} mp_rocket_t;

typedef struct {
    float t;    /* since ignition */
    float h, v; /* above the pad; up is positive */
    float mach; /* signed like v */
    float max_mach;
    bool canopy; /* a charge has put a canopy out */
    bool apogee;
    float apogee_t, apogee_h;
    bool landed;
} mp_state_t;

void mp_launch(mp_state_t *st);
void mp_step(mp_state_t *st, const mp_site_t *s, const mp_rocket_t *r, float dt);

/* The static ports' error, added to the true pressure. It is a fraction of
 * the dynamic pressure that depends on Mach: none below Mach 0.85, rising to
 * `below` just under Mach 1, stepping to `above` at Mach 1 and growing by
 * `slope` per unit of Mach beyond. `sign` +1 reads high (the rocket looks
 * lower), -1 low. */
typedef struct {
    float sign;
    float below, above, slope;
} mp_port_t;

float mp_port_error_pa(const mp_port_t *e, float mach, float static_pa);

/* An ejection charge pressurises the avionics bay: peak_pa, decaying with
 * tau_s, from the moment it fires. */
typedef struct {
    float peak_pa;
    float tau_s;
} mp_charge_t;

float mp_charge_pa(const mp_charge_t *c, float since_fire_s);

#endif
