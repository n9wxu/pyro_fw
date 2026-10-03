/*
 * The flight software: a state machine stepped once a loop, above the HAL.
 * The states and their transitions are in docs/flight_states.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_STATES_H
#define FLIGHT_STATES_H

#include "config.h"
#include "descent_phase.h"
#include "fire_control.h"
#include "flight_events.h"
#include "ground_test_seq.h"
#include "hal.h"
#include "landing_detector.h"
#include "launch_detector.h"
#include "mach_lock.h"
#include "pad_check.h"
#include "telemetry.h"
#include <stdbool.h>
#include <stdint.h>

/* Numbers reach the flight log, the telemetry and /api/status: new states are
 * appended, never inserted. */
typedef enum {
    BOOT_SETTLE = 0,
    BOOT_CONTINUITY,
    BOOT_CALIBRATE,
    PAD_IDLE,
    ASCENT,
    FALLING,
    DROGUE_DESCENT,
    CHUTE_DESCENT,
    LANDED,
    BOOT_SENSOR,
    FAULT,
    GROUND_TEST,
    STATE_COUNT
} flight_state_t;

#define SENSOR_PENDING 0xFFu

typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa;
    int32_t altitude_cm;
    uint8_t state;
    uint8_t under_thrust;
    uint8_t event;
} flight_sample_t;

#define FLIGHT_BUF_SIZE 64 /* the newest samples, for flight_save_csv() */

typedef struct flight_context_t {
    config_t config;
    fire_plan_t plan;
    bool refire_interval_limited, fire_gap_limited;
    flight_state_t current_state;
    uint32_t boot_timer;

    uint8_t sensor_type; /* 0: none answered; SENSOR_PENDING: still being brought up */
    bool storage_ok;
    uint16_t diag; /* DIAG_* of pad_check.h */

    /* The pad */
    uint32_t last_health_check;
    bool channel_ready[2];
    uint16_t channel_adc[2];
    bool announced;
    uint8_t announcement; /* beep_reason_t */
    bool usb_attached;
    bool test_mode;

    /* Resume [FLT-BROWN-01..07] */
    uint8_t reset_cause;
    uint8_t resume;      /* resume_verdict_t */
    uint8_t not_resumed; /* not_resumed_t */
    bool record_written;
    bool record_cleared;

    /* The flight */
    int32_t ground_pressure;
    uint32_t launch_time, armed_time, apogee_time, descent_start_time, landing_time;
    uint32_t last_sample; /* sample time of the newest sample */
    int32_t pressure_pa, altitude_cm, speed_cms;
    int32_t max_altitude_cm, max_speed_cms;
    float peak_pa; /* the lowest filtered pressure outside the Mach flag; 0: none yet */
    bool peak_lower_bound;
    bool arm_height_passed;
    bool pyros_armed;
    bool apogee_declared;
    bool under_thrust;
    bool sensor_stuck, sensor_lost;
    mach_lock_t mach;
    descent_phase_t descent;
    landing_detector_t landing;
    uint32_t landed_row_ms;

    /* Firing */
    fire_state_t fire;
    bool emergency_fire;
    bool channel_fault[2];
    bool channel_stayed_closed[2];
    bool verify_done[2];
    int32_t descent_speed_cms; /* in the pad's air, positive descending */

    telemetry_queue_t telemetry;
    uint16_t telemetry_seq;
    uint32_t last_telemetry;

    flight_sample_t flight_buffer[FLIGHT_BUF_SIZE];
    uint16_t buf_head, buf_count;

    /* Ground test [GND-TEST-05..13] */
    bool gt_held;
    uint32_t gt_held_since;
    bool gt_requested;
    gt_seq_t gt_seq;
} flight_context_t;

void flight_init(flight_context_t *ctx);
flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now);
void flight_update_outputs(flight_context_t *ctx, uint32_t now);
/* The storage writes the flight software owes; call where a write may run. */
void flight_storage_service(flight_context_t *ctx, uint32_t now);

void flight_set_usb_attached(flight_context_t *ctx, bool attached, uint32_t now); /* [USB-01..05] */
void flight_set_test_mode(flight_context_t *ctx, bool on, uint32_t now);          /* [USB-08] */

flight_context_t *flight_get_context(void);
flight_state_t flight_get_state(void);

/* Flight time: running while airborne, frozen at the landing [WEB-UI-04]. */
uint32_t flight_elapsed_ms(const flight_context_t *ctx, uint32_t now);
const char *flight_resume_text(const flight_context_t *ctx);

/* The simulator's export of the newest samples. */
int flight_save_csv(flight_context_t *ctx);

#endif
