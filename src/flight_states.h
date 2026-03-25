#ifndef FLIGHT_STATES_H
#define FLIGHT_STATES_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal.h"         /* hal_pressure_batch_t for flight_process_samples */
#include "ground_test.h" /* ground_test_ctx_t embedded in flight_context_t */

// System states
typedef enum {
    BOOT_INIT,       // load config, init buzzer
    BOOT_SETTLE,     // wait for sensors to stabilize
    BOOT_CONTINUITY, // check pyro circuits
    BOOT_CALIBRATE,  // establish ground reference
    PAD_IDLE,
    ASCENT,
    DESCENT,
    LANDED,
    STATE_COUNT
} flight_state_t;

// State machine events (drive transitions)
typedef enum {
    SEVT_NONE = 0,
    SEVT_DONE,
    SEVT_TIMER,
    SEVT_CAL_DONE,
    SEVT_LAUNCH,
    SEVT_ARMED,
    SEVT_APOGEE,
    SEVT_LANDING,
} state_event_t;

// Forward declare for function pointer types
struct flight_context_t;

// Transition table entry
typedef state_event_t (*detect_fn)(struct flight_context_t *ctx, uint32_t now);
typedef void (*action_fn)(struct flight_context_t *ctx, uint32_t now);

typedef struct {
    flight_state_t from;
    state_event_t event;
    flight_state_t to;
    action_fn action;
} transition_t;

/* Pyro firing modes — defined in config.h */

// Event types (stored in flight_sample_t.event)
#define EVT_NONE 0
#define EVT_LAUNCH 1
#define EVT_APOGEE 2
#define EVT_PYRO1_FIRE 3
#define EVT_PYRO2_FIRE 4
#define EVT_PYRO1_CONT 5 /* data1 = adc */
#define EVT_PYRO2_CONT 6 /* data1 = adc */
#define EVT_LANDING 7
#define EVT_STATE_CHANGE 8 /* data1 = new state */
#define EVT_ARMED 9
#define EVT_BOOT_DONE 10
#define EVT_PYRO1_FAULT 11
#define EVT_PYRO2_FAULT 12
#define EVT_PYRO1_NOPEN 13 /* post-fire verify: pyro didn't open */
#define EVT_PYRO2_NOPEN 14

// Flight sample (16 bytes)
typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa; /* or event data1 */
    int32_t altitude_cm; /* or event data2 */
    uint8_t state;
    uint8_t under_thrust;
    uint8_t event;      /* EVT_NONE = normal sample */
    uint8_t event_data; /* extra byte for event info */
} flight_sample_t;

/* config_t is defined in config.h */

// Flight context
/* Ring buffer size — holds samples for /api/flight.csv HTTP endpoint and
 * apogee-window replay (launch backdating). 64 entries × 16 bytes = 1KB.
 * In-flight record is owned by hal_log_sample() [v2-9]. */
#define FLIGHT_BUF_SIZE 64

typedef struct flight_context_t {
    flight_sample_t flight_buffer[FLIGHT_BUF_SIZE];
    uint16_t buf_head;
    uint16_t buf_tail;
    uint16_t buf_count;
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
    uint16_t pyro1_adc;
    uint16_t pyro2_adc;
    uint8_t pyro_firing;
    uint32_t pyro_fire_start;
    uint32_t pyro1_fire_time;
    uint32_t pyro2_fire_time;
    bool pyro1_fault; /* overcurrent detected during fire */
    bool pyro2_fault;
    bool pyro1_verify_fail; /* post-fire continuity still good (pyro didn't open) */
    bool pyro2_verify_fail;
    config_t config;
    flight_state_t current_state;
    int32_t filtered_pressure;
    bool filter_initialized;
    // Boot state fields
    uint32_t boot_timer;
    int cal_count;
    int32_t cal_sum;
    // PAD_IDLE state
    uint32_t last_cont_check;
    bool buzzer_started;
    bool landed_beep_started;
    bool csv_saved;
    // Safety features [DD-013, DD-016, DD-017]
    int32_t max_speed_cms;       // peak speed during ASCENT (for arming gate)
    uint32_t armed_time;         // when pyros were armed (for backup timer)
    uint32_t descent_start_time; // when DESCENT started (for landing timeout)
    int32_t pad_speed_cms;       // vertical speed during PAD_IDLE (for launch confirm)
    // Last continuity status beep code [GND-TEST-01]
    uint8_t last_status_code;
    // Ground test state machine [GND-TEST-01..04, DD-011]
    ground_test_ctx_t gt;
} flight_context_t;

// Flight init and dispatch
void flight_init(flight_context_t *ctx);
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);
void flight_update_outputs(flight_context_t *ctx, uint32_t now);
/* v2-8: batch processing — call once per 100ms pressure FIFO batch.
 * Iterates the batch at 50Hz (20ms spacing), feeds each sample into
 * dispatch_state(), and logs via hal_log_sample().
 * Triggers hal_log_start() on LAUNCH and hal_log_stop() on LANDED. */
void flight_process_samples(flight_context_t *ctx, const hal_pressure_batch_t *batch);
/* Legacy compat — redirects to config module */
#define parse_config_ini(buf, cfg) config_parse_ini(buf, cfg)
/* [DAT-06] One-shot ring-buffer dump — debug / HTTP endpoint use only.
 * The primary in-flight record is written by hal_log_sample() [v2-9]. */
int flight_save_csv(flight_context_t *ctx);
/* csv_flush_safe() / csv_flush_step() retired: hal_log owns the flight record [v2-9] */

// Telemetry
void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state);

// Helpers used by state functions
void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st);
int32_t filter_pressure(flight_context_t *ctx, int32_t raw_pressure, uint32_t dt_ms);
int32_t pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa);

#endif
