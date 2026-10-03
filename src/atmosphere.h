/*
 * The 1976 U.S. Standard Atmosphere, by pressure [SNS-EST-05, FLT-AIR-01].
 *
 * Flight comparisons are made in pressure; these turn a height or a speed
 * into pressure terms once, and pressure into a height for the operator.
 * Reference: U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

float atmos_temperature_k(float pressure_pa);
float atmos_altitude_m(float pressure_pa);
float atmos_pressure_pa(float altitude_m);

float atmos_height_above_m(float pressure_pa, float pad_pa);
float atmos_pressure_above_pa(float pad_pa, float height_m);

/* R T / g at this pressure: the metres per second in one unit of d(ln p)/dt. */
float atmos_scale_height_m(float pressure_pa);

/* sqrt(rho / rho_pad): what turns a speed at this pressure into the speed
 * the same body would have in the pad's air. */
float atmos_pad_air_ratio(float pressure_pa, float pad_pa);

#endif
