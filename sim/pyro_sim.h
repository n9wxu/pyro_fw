/*
 * Pyro MK1B Simulation Library — Public API
 *
 * This header provides a clean, stable interface for driving the
 * pyro flight computer as a black box from external projects.
 *
 * The pyro firmware is treated as an opaque system:
 *   1. Set inputs (time, pressure, continuity)
 *   2. Call sim_flight_tick()
 *   3. Read outputs (state, pyro fires, buzzer, telemetry)
 *
 * The caller is responsible for the physics engine. See sim_cli.c
 * or docs/physics.js for example physics implementations.
 *
 * Usage with CMake FetchContent:
 *
 *   FetchContent_Declare(pyro_fw
 *       GIT_REPOSITORY https://github.com/n9wxu/pyro_fw.git
 *       GIT_TAG main
 *   )
 *   FetchContent_MakeAvailable(pyro_fw)
 *   target_link_libraries(my_project PRIVATE pyro_sim)
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

/**
 * Initialize the flight computer. Optionally provide a config.ini
 * string (NULL or "" for defaults). Resets all internal state.
 */
void sim_flight_init(const char *config_ini);

/**
 * Advance the flight computer by one tick.
 * Call once per millisecond (or per simulation step).
 * Returns the current flight state (see flight_states.h for enum).
 */
int sim_flight_tick(uint32_t time_ms);

/* ── Inputs (set before calling sim_flight_tick) ──────────────────── */

/** Set simulation time (also set via sim_flight_tick parameter) */
void sim_set_time(uint32_t ms);

/** Set barometric pressure in Pascals (from your physics engine) */
void sim_set_pressure(float pa);

/** Set sensor type: 0=none, 1=ms5607, 2=bmp280 */
void sim_set_sensor_type(int type);

/** Set pyro continuity for channel 1 or 2 */
void sim_set_continuity(int ch, uint16_t adc, bool good, bool open);

/** Reset all simulation state to power-on defaults */
void sim_reset(void);

/* ── Outputs (read after calling sim_flight_tick) ─────────────────── */

/** Current flight state: 0=BOOT_INIT..7=LANDED (see flight_states.h) */
int sim_flight_state(void);

/** Current altitude in centimeters (filtered, above ground) */
int32_t sim_flight_altitude_cm(void);

/** Maximum altitude reached in centimeters */
int32_t sim_flight_max_alt_cm(void);

/** Vertical speed in cm/s (positive=up, negative=down) */
int32_t sim_flight_vspeed_cms(void);

/** Filtered pressure in Pascals */
int32_t sim_flight_pressure(void);

/** True if pyro channel 1 has fired */
bool sim_flight_pyro1_fired(void);

/** True if pyro channel 2 has fired */
bool sim_flight_pyro2_fired(void);

/** True if pyros are armed (speed < 10 m/s, ascending) */
bool sim_flight_armed(void);

/** Number of flight data samples in the ring buffer */
int sim_flight_samples(void);

/** Timestamp when launch was detected (ms) */
uint32_t sim_flight_launch_time(void);

/** Save flight data to in-memory CSV (call after landing) */
void sim_flight_save_csv(void);

/* ── HAL accessors (low-level, for physics feedback) ──────────────── */

/** Clear the pyro firing flag (call before each tick) */
void sim_clear_pyro_firing(void);

/** Number of times any pyro has been fired */
int sim_get_pyro_fire_count(void);

/** Channel number of the most recent pyro fire (1 or 2) */
uint8_t sim_get_pyro_last_channel(void);

/** Current buzzer on/off state */
bool sim_get_buzzer_state(void);

/** Accumulated telemetry output (NMEA sentences) */
const char *sim_get_telemetry(void);

/** Length of accumulated telemetry in bytes */
int sim_get_telemetry_len(void);

/** Clear accumulated telemetry buffer */
void sim_clear_telemetry(void);

#ifdef __cplusplus
}
#endif

#endif /* PYRO_SIM_H */
