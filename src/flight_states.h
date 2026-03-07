#ifndef FLIGHT_STATES_H
#define FLIGHT_STATES_H

#include <stdint.h>
#include <stdbool.h>

// System states (boot + flight)
typedef enum {
    // Boot sequence
    BOOT_FILESYSTEM,
    BOOT_I2C_SETTLE,
    BOOT_SENSOR_DETECT,
    BOOT_PYRO_INIT,
    BOOT_CONTINUITY,
    BOOT_STABILIZE,
    BOOT_CALIBRATE,
    BOOT_MDNS,
    // Flight sequence
    PAD_IDLE,
    ASCENT,
    DESCENT,
    LANDED
} flight_state_t;

// Pyro firing modes
typedef enum { PYRO_MODE_FALLEN = 1, PYRO_MODE_AGL, PYRO_MODE_SPEED, PYRO_MODE_DELAY } pyro_mode_t;

// Flight sample
typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa;
    int32_t altitude_cm;
    uint8_t state;
    uint8_t under_thrust;
    uint8_t padding[2];
} flight_sample_t;

// Config
typedef struct {
    char id[9];
    char name[9];
    uint8_t pyro1_mode;
    uint16_t pyro1_value;
    uint8_t pyro2_mode;
    uint16_t pyro2_value;
    uint8_t units;
    uint8_t beep_mode;
} config_t;

// Flight context
typedef struct flight_context_t {
    flight_sample_t flight_buffer[4096];
    uint16_t buf_head;
    uint16_t buf_tail;
    uint16_t buf_count;
    uint16_t apogee_protect_idx;
    bool apogee_protected;
    int32_t ground_pressure;
    int32_t max_altitude;
    int32_t last_altitude;
    int32_t vertical_speed_cms;
    int32_t prev_vertical_speed_cms;
    uint32_t launch_time;
    uint32_t apogee_time;
    uint32_t last_sample;
    uint32_t last_telemetry;
    uint32_t landing_stable_since;
    bool pyro1_fired;
    bool pyro2_fired;
    bool pyro1_continuity_good;
    bool pyro2_continuity_good;
    bool pyros_armed;
    bool apogee_detected;
    bool under_thrust;
    uint16_t telemetry_seq;
    uint8_t pyro_firing;
    uint32_t pyro_fire_start;
    uint32_t pyro1_fire_time;
    uint32_t pyro2_fire_time;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    bool filter_initialized;
    // Boot state fields
    uint32_t boot_timer;
    int cal_count;
    int32_t cal_sum;
} flight_context_t;

// State dispatch
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);

// Telemetry
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state);

// Helpers used by state functions
void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st);
int32_t filter_pressure(flight_context_t *ctx, int32_t raw_pressure, uint32_t dt_ms);
int32_t pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa);

#endif
