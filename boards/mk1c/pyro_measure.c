/*
 * SPDX-License-Identifier: MIT
 */
#include "pyro_measure.h"
#include "board_pins.h"
#include "board_support.h"
#include <string.h>

#define TRACK_BIAS_MS 8
#define TRACK_PERIOD_MS 500

static struct {
    bool biased;
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

void tracking_init(void) {
    memset(&tracking, 0, sizeof(tracking));
}

bool tracking_step(uint32_t now_ms) {
    if (!tracking.biased) {
        if (!deadline_reached(now_ms, tracking.due_ms))
            return false;
        bias_bus(true);
        tracking.start_ms = now_ms;
        tracking.due_ms = now_ms + TRACK_PERIOD_MS;
        return false;
    }
    if (now_ms - tracking.start_ms < TRACK_BIAS_MS)
        return false;
    tracking.result.bus = adc_read_median3(BOARD_ADC_CH_BUS);
    tracking.result.a = adc_read_median3(BOARD_ADC_CH_A);
    tracking.result.b = adc_read_median3(BOARD_ADC_CH_B);
    tracking.result.valid = true;
    bias_bus(false);
    return true;
}

void tracking_abandon(void) {
    bias_bus(false);
}

void tracking_run_next_at(uint32_t now_ms) {
    tracking.due_ms = now_ms;
}

const tracking_t *tracking_result(void) {
    return &tracking.result;
}
