/*
 * FatFs's OS layer: one static FreeRTOS mutex per volume [DD-076].
 *
 * Before the scheduler starts only core0 runs -- the card is mounted and the
 * configuration mirrored then -- and taking the lock is a no-op. Never from
 * the flight task: a refusal there reads as a timeout, FR_TIMEOUT.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "rtos_tasks.h"

static SemaphoreHandle_t vol_mutex[FF_VOLUMES + 1];
static StaticSemaphore_t vol_mutex_buf[FF_VOLUMES + 1];

int ff_mutex_create(int vol) {
    if (!vol_mutex[vol])
        vol_mutex[vol] = xSemaphoreCreateMutexStatic(&vol_mutex_buf[vol]);
    return vol_mutex[vol] != NULL;
}

void ff_mutex_delete(int vol) {
    (void)vol; /* static: kept for the next mount */
}

int ff_mutex_take(int vol) {
    if (!rtos_running())
        return 1;
    if (rtos_in_flight_task())
        return 0;
    return xSemaphoreTake(vol_mutex[vol], pdMS_TO_TICKS(FF_FS_TIMEOUT)) == pdTRUE;
}

void ff_mutex_give(int vol) {
    if (rtos_running())
        xSemaphoreGive(vol_mutex[vol]);
}
