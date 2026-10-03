/*
 * What the flight software's files share. Not an interface: nothing outside
 * src/flight_*.c includes this.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_INTERNAL_H
#define FLIGHT_INTERNAL_H

#include "flight_states.h"

typedef enum {
    SEVT_NONE = 0,
    SEVT_DONE,
    SEVT_TIMER,
    SEVT_CAL_DONE,
    SEVT_LAUNCH,
    SEVT_ARMED,
    SEVT_APOGEE,
    SEVT_DROGUE,   /* descent has steadied at a drogue's rate */
    SEVT_CHUTE,    /* descent has steadied at a main's rate */
    SEVT_FREEFALL, /* a canopy that was working has stopped */
    SEVT_LANDING,
    SEVT_FAULT,
    SEVT_RESUME_ASCENT,
    SEVT_RESUME_DESCENT,
    SEVT_GROUND_TEST,
} state_event_t;

/* One detector per state, and the actions its transitions run. */
state_event_t flight_detect_boot_settle(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_boot_sensor(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_boot_continuity(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_boot_calibrate(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_fault(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_pad_idle(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_ascent(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_falling(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_drogue_descent(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_chute_descent(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_landed(flight_context_t *ctx, uint32_t now);
state_event_t flight_detect_ground_test(flight_context_t *ctx, uint32_t now);

void flight_action_fault(flight_context_t *ctx, uint32_t now);
void flight_action_start_calibration(flight_context_t *ctx, uint32_t now);
void flight_action_on_pad(flight_context_t *ctx, uint32_t now);
void flight_action_launch(flight_context_t *ctx, uint32_t now);
void flight_action_armed(flight_context_t *ctx, uint32_t now);
void flight_action_apogee(flight_context_t *ctx, uint32_t now);
void flight_action_resumed_descent(flight_context_t *ctx, uint32_t now);
void flight_action_landing(flight_context_t *ctx, uint32_t now);
void flight_action_ground_test(flight_context_t *ctx, uint32_t now);

/* ── Shared by the detectors ──────────────────────────────────────── */

bool flight_grounded_on_usb(const flight_context_t *ctx); /* [USB-01, USB-08] */
bool flight_state_is_airborne(flight_state_t state);
void flight_make_plan(flight_context_t *ctx);
void flight_read_pyro_health(flight_context_t *ctx);

/* The newest sample, into the context, the ring and the flight log. */
void flight_take_sample(flight_context_t *ctx, const pp_sample_t *s, flight_state_t state);
void flight_log_event(flight_context_t *ctx, uint8_t event);
void flight_note_sensor(flight_context_t *ctx, const pp_sample_t *s, uint32_t now);
void flight_note_no_sample(flight_context_t *ctx, uint32_t now);

void flight_say_fault(void);
int32_t flight_to_units(int32_t cm, uint8_t units);

/* Pulse a channel now, record it and queue its report [PYR-FIRE-01]. */
void flight_deliver_pulse(flight_context_t *ctx, fire_command_t command, const fire_inputs_t *in);
fire_inputs_t flight_fire_inputs(const flight_context_t *ctx, const pp_sample_t *s, uint32_t now_ms);

#endif
