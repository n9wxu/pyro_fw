/*
 * Core1's frame. See core1_frame.h.
 *
 * The flight task writes the frame's start; the storage task writes when it
 * gave its slot up. The Lua task reads both and writes nothing here but its
 * skips. Every variable has one writer, so nothing takes a lock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "core1_frame.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/sync.h"
#include "pico/time.h"

static TaskHandle_t storage_h, lua_h;
static volatile uint32_t frame_t0;    /* the flight task */
static volatile uint32_t writer_done; /* the storage task: the frame it gave up */
static volatile bool lua_waiting;     /* the Lua task */
static volatile uint32_t writer_max_us, writer_overruns, lua_skipped;

void core1_frame_attach_storage(TaskHandle_t h) {
    storage_h = h;
}

void core1_frame_attach_lua(TaskHandle_t h) {
    lua_h = h;
}

void core1_frame_begin(uint32_t t0_us) {
    frame_t0 = t0_us;
    __dmb();
    if (storage_h)
        xTaskNotifyGive(storage_h);
}

uint32_t core1_frame_t0(void) {
    return frame_t0;
}

void core1_frame_writer_done(void) {
    uint32_t t0 = frame_t0;
    if (writer_done == t0)
        return;
    uint32_t held = time_us_32() - t0;
    if (held < LOOP_PERIOD_US && held > writer_max_us)
        writer_max_us = held;
    if (held > FRAME_WRITER_US && held < LOOP_PERIOD_US)
        writer_overruns++;
    writer_done = t0;
    __dmb();
    if (lua_waiting && lua_h)
        xTaskNotifyGive(lua_h);
}

bool core1_frame_wait(uint32_t timeout_ms) {
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) != 0;
}

uint32_t core1_frame_lua_slot(uint32_t t0) {
    lua_waiting = true;
    __dmb();
    for (;;) {
        uint32_t now = time_us_32();
        uint32_t left = frame_writer_left_us(t0, now);
        if (writer_done == t0 || left == 0)
            break;
        /* A tick of the scheduler is a millisecond: wake on the writer's
         * word, or at the slot's end at the latest. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((left + 999u) / 1000u));
    }
    lua_waiting = false;
    uint32_t budget = frame_lua_budget_us(t0, time_us_32());
    if (!budget)
        lua_skipped++;
    return budget;
}

void core1_frame_stats(core1_frame_stats_t *out) {
    out->writer_max_us = writer_max_us;
    out->writer_overruns = writer_overruns;
    out->lua_skipped = lua_skipped;
}
