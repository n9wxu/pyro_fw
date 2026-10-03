/*
 * When each pyro channel is pulsed [PYR-MODE-01..06, PYR-DEPLOY-02,
 * PYR-REFIRE-01, FLT-EMRG-01, DD-082].
 *
 * A pure decision: given the plan, the filtered state and what has been
 * pulsed so far, which channel to pulse now, if any. The caller delivers the
 * pulse and reports it back.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FIRE_CONTROL_H
#define FIRE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* What the configuration and the board ask of the two channels, in SI units.
 * A speed of zero disables its rule. */
typedef struct {
    bool enabled[2];
    uint8_t mode[2];     /* pyro_mode_t */
    float trigger_m[2];  /* AGL height or FALLEN distance */
    float trigger_ms[2]; /* SPEED, metres per second of descent */
    uint32_t trigger_delay_ms[2];
    float refire_ms[2];
    float emergency_ms;
    uint32_t refire_interval_ms;
    uint32_t fire_gap_ms;
} fire_plan_t;

typedef struct {
    uint32_t now_ms; /* sample time */
    bool apogee_declared;
    uint32_t apogee_ms;
    bool state_known; /* false while the sensor gives no data [SNS-PRES-10, SNS-PRES-11] */
    float pressure_pa;
    float rate; /* d(ln p)/dt: positive descending */
    float pad_pa;
    float peak_pa;     /* the lowest pressure of the flight */
    bool pulse_active; /* a channel is energised now */
} fire_inputs_t;

typedef enum { FIRE_NONE = 0, FIRE_FIRST, FIRE_REFIRE, FIRE_EMERGENCY } fire_kind_t;

typedef struct {
    uint8_t channel; /* 1 or 2; 0 with FIRE_NONE */
    fire_kind_t kind;
} fire_command_t;

typedef struct {
    bool fired;
    uint16_t pulses;
    uint32_t last_pulse_ms;
    uint32_t last_pulse_end_ms;
    bool pulse_ended;
} fire_channel_t;

typedef struct {
    fire_channel_t channel[2];
    uint8_t active_channel; /* 0: none */
    bool emergency;
    /* [PYR-MODE-06] The state just before the newest pulse: for a while after
     * it, the rules see nothing lower or faster than free fall from here. */
    bool charge_guard;
    uint32_t guard_from_ms;
    float guard_log_pressure, guard_rate;
} fire_state_t;

void fire_control_reset(fire_state_t *s);
fire_command_t fire_control_step(fire_state_t *s, const fire_plan_t *plan, const fire_inputs_t *in);
void fire_control_pulse_started(fire_state_t *s, uint8_t channel, const fire_inputs_t *in);

/* The descent speed the re-fire and emergency rules compare, in the pad's
 * air [FLT-AIR-01], metres per second, positive descending. */
float fire_control_descent_speed_ms(const fire_state_t *s, const fire_inputs_t *in);

#endif
