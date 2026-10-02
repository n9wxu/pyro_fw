/*
 * The storage task: pinned to core1 at P [DD-073].
 *
 * Woken by the flight task each period, it writes what the flight task
 * handed over -- the flight log's ring, the pad marker -- and whatever the
 * board queued. Each flash operation inside stops the system for itself alone
 * (flash_op.h); between them the flight task runs.
 *
 * It checks in every pass. On the ground, a storage task that stops checking
 * in stops the watchdog being fed; in flight it is counted and the flight
 * goes on (rtos_tasks.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "FreeRTOS.h"
#include "task.h"
#include "hal.h"
#include "hal_storage.h"
#include "flight_states.h"
#include "loop_period.h"
#include "rtos_tasks.h"

/* A new image commits once the flight task has turned over this many periods
 * and this task has run: five seconds of every task alive. A commit that
 * fails is tried again this often, until it lands: an image left uncommitted
 * rolls back on its next reboot. */
#define COMMIT_PERIODS (5000u / LOOP_PERIOD_MS)
#define COMMIT_RETRY_MS 1000u

void storage_task(void *arg) {
    (void)arg;
    extern volatile uint32_t loop_count;
    bool committed = false;
    uint32_t commit_due_ms = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2u * LOOP_PERIOD_MS));
        uint32_t now = hal_time_ms();
        storage_checkin(now);
        if (!committed && loop_count >= COMMIT_PERIODS && (int32_t)(now - commit_due_ms) >= 0) {
            committed = hal_firmware_commit();
            commit_due_ms = now + COMMIT_RETRY_MS;
        }
        hal_storage_service(now);
        flight_context_t *ctx = flight_get_context();
        if (ctx)
            flight_flash_service(ctx, now);
    }
}
