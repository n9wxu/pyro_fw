/*
 * The flight computer as a black box, for a physics engine to drive: set the
 * inputs, call sim_flight_tick(), read the outputs. sim/sim_cli.c and
 * docs/sim.html (through docs/wasm/pyro-sim.js) are the two drivers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PYRO_SIM_H
#define PYRO_SIM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ────────────────────────────────────────────────────── */

/* Power-on reset, then boot with this config.ini text (NULL or "" for the
 * defaults). */
void sim_flight_init(const char *config_ini);

/* One pass of the firmware's loop at time_ms. Returns the flight_state_t
 * (src/flight_states.h). */
int sim_flight_tick(uint32_t time_ms);

void sim_reset(void);

/* ── Inputs ───────────────────────────────────────────────────────── */

void sim_set_time(uint32_t ms);
void sim_set_pressure(float pa);
void sim_set_sensor_type(int type); /* a pressure_sensor_type_t (src/pressure_sensor.h) */
void sim_set_continuity(int ch, uint16_t adc, bool good, bool open);

/* ── Outputs ──────────────────────────────────────────────────────── */

int sim_flight_state(void); /* a flight_state_t */
int32_t sim_flight_altitude_cm(void);
int32_t sim_flight_max_alt_cm(void);
int32_t sim_flight_vspeed_cms(void);
int32_t sim_flight_pressure(void);
bool sim_flight_pyro1_fired(void);
bool sim_flight_pyro2_fired(void);
bool sim_flight_armed(void); /* [FLT-ASC-04] */
int sim_flight_samples(void);
uint32_t sim_flight_launch_time(void);
void sim_flight_save_csv(void);

/* sim_flight_tick() clears the firing flag itself; a driver that steps the
 * firmware some other way, as a replay does, calls it. */
void sim_clear_pyro_firing(void);
int sim_get_pyro_fire_count(void);
uint8_t sim_get_pyro_last_channel(void);
bool sim_get_buzzer_state(void);
const char *sim_get_telemetry(void);
int sim_get_telemetry_len(void);
void sim_clear_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif /* PYRO_SIM_H */
