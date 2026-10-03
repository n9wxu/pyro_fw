/*
 * SPDX-License-Identifier: MIT
 */
#include "pyro_measure.h"
#include "board_pins.h"
#include "board_support.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include <string.h>

/* See THEORY_OF_OPERATION.md "Presence test". */
#define TRACK_BIAS_US 8000u
#define TRACK_PERIOD_MS 500
#define NO_ALARM (-1)

static struct {
    int alarm; /* the hardware alarm that ends the pulse, or NO_ALARM */
    bool alarm_asked;
    volatile bool biased;
    volatile bool result_is_new;
    uint32_t start_ms, due_ms;
    tracking_t result;
} tracking;

void read_quiescent(quiescent_t *q) {
    q->vbat = adc_read_median3(BOARD_ADC_CH_VBAT);
    q->bus = adc_read_median3(BOARD_ADC_CH_BUS);
    q->a = adc_read_median3(BOARD_ADC_CH_A);
    q->b = adc_read_median3(BOARD_ADC_CH_B);
}

static void bias_bus(bool on) {
    gpio_put(BOARD_PIN_BIAS_BUS, on);
    tracking.biased = on;
}

/* The end of the pulse: read under the bias, then let it go. Runs in the
 * alarm's interrupt, or from the loop on a board with no alarm free. */
static void take_reading(void) {
    if (!tracking.biased)
        return; /* abandoned for a fire */
    tracking.result.bus = adc_read_median3(BOARD_ADC_CH_BUS);
    tracking.result.a = adc_read_median3(BOARD_ADC_CH_A);
    tracking.result.b = adc_read_median3(BOARD_ADC_CH_B);
    tracking.result.valid = true;
    bias_bus(false);
    tracking.result_is_new = true;
}

static void pulse_end_isr(uint alarm) {
    (void)alarm;
    take_reading();
}

/* Claimed from the loop, so the alarm's interrupt belongs to the loop's core
 * and never runs beside it. */
static bool have_alarm(void) {
    if (!tracking.alarm_asked) {
        tracking.alarm_asked = true;
        tracking.alarm = hardware_alarm_claim_unused(false);
        if (tracking.alarm != NO_ALARM)
            hardware_alarm_set_callback((uint)tracking.alarm, pulse_end_isr);
    }
    return tracking.alarm != NO_ALARM;
}

void tracking_init(void) {
    int alarm = tracking.alarm;
    bool asked = tracking.alarm_asked;
    memset(&tracking, 0, sizeof(tracking));
    tracking.alarm = asked ? alarm : NO_ALARM;
    tracking.alarm_asked = asked;
}

static void begin_pulse(uint32_t now_ms) {
    bias_bus(true);
    tracking.start_ms = now_ms;
    tracking.due_ms = now_ms + TRACK_PERIOD_MS;
    if (have_alarm())
        hardware_alarm_set_target((uint)tracking.alarm, make_timeout_time_us(TRACK_BIAS_US));
}

bool tracking_step(uint32_t now_ms) {
    if (tracking.result_is_new) {
        tracking.result_is_new = false;
        return true;
    }
    if (tracking.biased) {
        if (!have_alarm() && now_ms - tracking.start_ms >= TRACK_BIAS_US / 1000u)
            take_reading();
        return false;
    }
    if (deadline_reached(now_ms, tracking.due_ms))
        begin_pulse(now_ms);
    return false;
}

void tracking_abandon(void) {
    if (tracking.alarm != NO_ALARM)
        hardware_alarm_cancel((uint)tracking.alarm);
    bias_bus(false);
    tracking.result_is_new = false;
}

bool tracking_pulse_active(void) {
    return tracking.biased;
}

void tracking_run_next_at(uint32_t now_ms) {
    tracking.due_ms = now_ms;
}

const tracking_t *tracking_result(void) {
    return &tracking.result;
}
