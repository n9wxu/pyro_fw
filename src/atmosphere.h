/*
 * The 1976 U.S. Standard Atmosphere [SNS-EST-05, FLT-AIR-01], and the air
 * over the pad.
 *
 * An altitude is the standard atmosphere's height of a pressure: the
 * estimators filter it. A height is true metres above the pad, in air as
 * warm as the pad was: every decision is made on it [DD-114].
 * Reference: U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

float atmos_altitude_m(float pressure_pa);
float atmos_pressure_pa(float altitude_m);

float atmos_temperature_at_k(float altitude_m);

/* Metres of altitude per pascal at this altitude, whose pressure is
 * pressure_pa. */
float atmos_m_per_pa(float altitude_m, float pressure_pa);

/* The standard atmosphere offset by the pad's temperature, at every height
 * the same offset. */
typedef struct {
    float pad_pa, pad_altitude_m;
    float offset_k; /* the pad's temperature less the standard's there */
    float pad_k;
} air_t;

/* pad_k of 0 or less: the standard atmosphere itself. */
air_t air_over_pad(float pad_pa, float pad_k);
float air_height_m(const air_t *a, float altitude_m);
/* Metres of height per metre of altitude, here: what turns an altitude's
 * speed into a true one. */
float air_scale(const air_t *a, float altitude_m);
/* sqrt(rho / rho_pad): what turns a speed here into the speed the same body
 * would have in the pad's air. */
float air_pad_ratio(const air_t *a, float altitude_m);
/* The altitude at this height above the pad. */
float air_altitude_m(const air_t *a, float height_m);

#endif
