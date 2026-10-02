/*
 * SPDX-License-Identifier: MIT
 */
#include "pyro_sequence.h"
#include "arm_pump.h"
#include "board_pins.h"
#include "pyro_faults.h"
#include "pyro_sense.h"
#include "board_support.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

extern void hal_telemetry_send(const char *sentence);

/* See THEORY_OF_OPERATION.md "Firing sequence". */
#define FIRE_AT_TENTHS_OF_PACK 9
#define PUMP_TO_ENABLE_US 400
#define RAMP_MV_PER_MS 890
#define ENABLE_COLLAPSE_MS 10
#define HOLD_MAX_MS 30
#define FLAT_BUS_COUNTS 25
#define PACK_UVLO_COUNTS 1239 /* 3.0 V */
#define BLEED_BELOW_HOT_MS 100

static const uint8_t gate_pin[2] = {BOARD_PIN_FIRE_A, BOARD_PIN_FIRE_B};

static struct {
    fire_step_t step;
    uint8_t channel; /* 1 or 2 */
    uint32_t armed_ms, timeout_ms, fired_ms, disarmed_ms;
    uint16_t last_hold_bus;
    bool fired_since_tracking[2];
} seq;

static void report(const char *what, const quiescent_t *q) {
    char line[112];
    snprintf(line, sizeof(line), "!PYRO %s ch=%u bus=%u vbat=%u\r\n", what, seq.channel, q->bus, q->vbat);
    hal_telemetry_send(line);
}

static uint32_t precharge_timeout_ms(const quiescent_t *q) {
    uint32_t ramp_us = (counts_to_node_mv(q->vbat) * FIRE_AT_TENTHS_OF_PACK * 100u) / RAMP_MV_PER_MS;
    uint32_t expected_us = PUMP_TO_ENABLE_US + ramp_us;
    return (expected_us * 3u / 2u + 999u) / 1000u;
}

static bool bus_charged(const quiescent_t *q) {
    return 10u * (uint32_t)q->bus >= FIRE_AT_TENTHS_OF_PACK * (uint32_t)q->vbat;
}

static uint16_t bus_change(const quiescent_t *q) {
    uint16_t d = q->bus > seq.last_hold_bus ? q->bus - seq.last_hold_bus : seq.last_hold_bus - q->bus;
    seq.last_hold_bus = q->bus;
    return d;
}

static void disarm(uint32_t now_ms) {
    arm_pump_stop();
    seq.disarmed_ms = now_ms;
}

void sequence_init(void) {
    memset(&seq, 0, sizeof(seq));
}

const char *sequence_refusal(uint8_t channel, const quiescent_t *q, const tracking_t *t) {
    if (channel != 1 && channel != 2)
        return "no such channel";
    if (sequence_firing())
        return "a fire is in progress";
    if (faults_latched() != FAULT_NONE)
        return "latched fault";
    if (!t->valid)
        return "no tracking result yet";
    if (seq.fired_since_tracking[channel - 1])
        return "no tracking test since this channel fired";
    if (track_channel(channel == 1 ? t->a : t->b, t->bus) != TRACK_PRESENT)
        return "the channel does not read present";
    if (q->vbat < PACK_UVLO_COUNTS)
        return "pack below UVLO";
    if (seq.step == STEP_IDLE && bus_is_hot(q))
        return "the bus is live with nothing armed";
    return NULL;
}

void sequence_arm(uint8_t channel, const quiescent_t *q) {
    seq.channel = channel;
    seq.armed_ms = to_ms_since_boot(get_absolute_time());
    seq.timeout_ms = precharge_timeout_ms(q);
    seq.step = STEP_PRECHARGE;
    arm_pump_start();
    arm_pump_feed();
    report("ARM", q);
}

static void precharge_step(uint32_t now_ms, const quiescent_t *q) {
    if (faults_latched() != FAULT_NONE) {
        disarm(now_ms);
        seq.step = STEP_DRAIN;
        report("ABORT", q);
    } else if (bus_charged(q)) {
        gpio_put(gate_pin[seq.channel - 1], 1);
        disarm(now_ms);
        seq.fired_ms = now_ms;
        seq.last_hold_bus = q->bus;
        seq.step = STEP_HOLD;
        report("FIRE", q);
    } else if (deadline_reached(now_ms, seq.armed_ms + seq.timeout_ms)) {
        disarm(now_ms);
        faults_latch(FAULT_PRECHARGE_TIMEOUT);
        seq.step = STEP_DRAIN;
        report("ABORT precharge timeout", q);
    } else {
        arm_pump_feed();
    }
}

static void hold_step(uint32_t now_ms, const quiescent_t *q) {
    bool flat = bus_change(q) <= FLAT_BUS_COUNTS;
    if ((deadline_reached(now_ms, seq.fired_ms + ENABLE_COLLAPSE_MS) && flat) ||
        deadline_reached(now_ms, seq.fired_ms + HOLD_MAX_MS)) {
        gpio_put(gate_pin[seq.channel - 1], 0);
        seq.fired_since_tracking[seq.channel - 1] = true;
        seq.step = STEP_DRAIN;
    }
}

static void drain_step(uint32_t now_ms, const quiescent_t *q) {
    if (q->bus <= COLD_NODE_MAX_COUNTS) {
        seq.step = STEP_IDLE;
        tracking_run_next_at(now_ms);
    }
}

void sequence_step(uint32_t now_ms, const quiescent_t *q) {
    switch (seq.step) {
    case STEP_PRECHARGE:
        precharge_step(now_ms, q);
        break;
    case STEP_HOLD:
        hold_step(now_ms, q);
        break;
    case STEP_DRAIN:
        drain_step(now_ms, q);
        break;
    case STEP_IDLE:
        break;
    }
}

fire_step_t sequence_current_step(void) {
    return seq.step;
}

bool sequence_firing(void) {
    return seq.step == STEP_PRECHARGE || seq.step == STEP_HOLD;
}

bool sequence_charged_the_bus(uint32_t now_ms) {
    if (sequence_firing())
        return true;
    return seq.step == STEP_DRAIN && (int32_t)(now_ms - seq.disarmed_ms) < BLEED_BELOW_HOT_MS;
}

/* From the arm until the presence test after the fire, if there is one. */
bool sequence_verdict_pending(uint8_t channel) {
    return seq.fired_since_tracking[channel - 1] || (seq.step != STEP_IDLE && seq.channel == channel);
}

static void report_verdict(uint8_t channel, uint16_t counts, uint16_t bus) {
    track_t t = track_channel(counts, bus);
    char line[112];
    snprintf(line, sizeof(line), "!PYRO F10 ch=%u %s (%u, bus %u)\r\n", channel,
             t == TRACK_OPEN      ? "open: fired"
             : t == TRACK_PRESENT ? "still present: misfire, treat as live"
                                  : "unverified: the bus did not rise",
             counts, bus);
    hal_telemetry_send(line);
}

void sequence_verify_fired(const tracking_t *t) {
    for (uint8_t i = 0; i < 2; i++) {
        if (seq.fired_since_tracking[i]) {
            seq.fired_since_tracking[i] = false;
            report_verdict((uint8_t)(i + 1), i == 0 ? t->a : t->b, t->bus);
        }
    }
}
