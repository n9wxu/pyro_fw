/*
 * The rocket the simulators fly: a vertical point mass with a constant-thrust
 * burn, linear chute damping, and the U.S. Standard Atmosphere, 1976.
 * Used by the WASM module (scripts/build_wasm.sh) and the CLI simulator
 * (sim/sim_cli.c).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PHYSICS_H
#define PHYSICS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562): its defining constants. */
#define PHYS_G0 9.80665f                   /* m/s², standard gravity */
#define PHYS_R_STAR 8.31432                /* J/(mol·K), the standard's own gas constant */
#define PHYS_M0 0.0289644                  /* kg/mol, sea-level mean molar mass */
#define PHYS_R_AIR (PHYS_R_STAR / PHYS_M0) /* 287.053 J/(kg·K) */
#define PHYS_SEA_LEVEL_PA 101325.0f

/* Altitudes are geopotential metres. Defined to the top of the standard's
 * seventh layer, 84 852 m; above it the last layer's temperature is held,
 * which the standard does not define. */
float physics_pressure_pa(float alt_m);
float physics_temperature_k(float alt_m);
float physics_density_kg_m3(float alt_m);

/* Descent drag is a damping term, a = k·(ρ(h)/ρ0)·|v|, with k in 1/s: the
 * terminal speed at sea level is g/k. It is not ½ρv²CdA/m; the profiles are
 * tuned to land at a plausible rate, not to fly a particular airframe. */
typedef struct {
    float alt_m;
    float vel_ms; /* positive up */
    float thrust_accel_ms2;
    float burn_time_s;
    float apogee_m;
    float drogue_damping_per_s;
    float main_damping_per_s;
    float ballistic_damping_per_s;
    bool drogue_deployed;
    bool main_deployed;
    bool on_ground;
} physics_state_t;

#define PHYS_STEP_S 0.001f

/* Chooses a thrust for the target apogee and solves the burn time that
 * reaches it in vacuum. */
void physics_init(physics_state_t *ps, float target_alt_m);
void physics_reset(physics_state_t *ps);
void physics_set_profile(physics_state_t *ps, float thrust_accel_ms2, float burn_time_s);
void physics_set_damping(physics_state_t *ps, float drogue_per_s, float main_per_s, float ballistic_per_s);
void physics_step(physics_state_t *ps, float since_launch_s);
void physics_deploy_drogue(physics_state_t *ps);
void physics_deploy_main(physics_state_t *ps);

/* The same, over one global state, for the WASM boundary (no structs). */
void physics_wasm_init(float target_alt_m);
void physics_wasm_reset(void);
void physics_wasm_step(float since_launch_s);
void physics_wasm_deploy_drogue(void);
void physics_wasm_deploy_main(void);
float physics_wasm_alt_m(void);
float physics_wasm_vel_ms(void);
float physics_wasm_pressure_pa(void);
float physics_wasm_apogee_m(void);
int physics_wasm_on_ground(void);
int physics_wasm_drogue_deployed(void);
int physics_wasm_main_deployed(void);

#ifdef __cplusplus
}
#endif

#endif /* PHYSICS_H */
