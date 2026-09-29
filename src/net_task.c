/*
 * The net task: TinyUSB, lwIP and HTTP, pinned to core1 at P [DD-073].
 *
 * It owns the USB interrupt as well as the stack: tud_init() runs here, so the
 * interrupt lands on core1 beside tud_task(). TinyUSB built OPT_OS_NONE guards
 * its event queue by masking the interrupt on the calling core only, so the
 * two must share a core.
 *
 * It sleeps until TinyUSB queues an event (tud_event_hook_cb) or a tick
 * passes, for lwIP's timers and HTTP's rings.
 *
 * SPDX-License-Identifier: MIT
 */
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "hal.h"
#include "http_server.h"
#include "http_work.h"
#include "loop_period.h"
#include "rtos_tasks.h"
#include "pico/time.h"

static TaskHandle_t net_h;

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    if (!net_h)
        return;
    if (in_isr) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(net_h, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotifyGive(net_h);
    }
}

/* Units run back to back while any has work, a few at a time between
 * transport passes so bytes keep moving. The tick shares core1 with Lua and
 * storage meanwhile: nothing here needs a budget to protect the flight. */
#define UNITS_PER_PASS 4

void net_task(void *arg) {
    (void)arg;
    net_h = xTaskGetCurrentTaskHandle();
    tud_init(BOARD_TUD_RHPORT);
    uint32_t period_t0 = time_us_32();
    for (;;) {
        hal_platform_service();
        if ((uint32_t)(time_us_32() - period_t0) >= LOOP_PERIOD_US) {
            period_t0 = time_us_32();
            http_server_period();
        }
        for (int i = 0; i < UNITS_PER_PASS; i++) {
            if (!http_server_work((int32_t)LOOP_PERIOD_US))
                break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}
