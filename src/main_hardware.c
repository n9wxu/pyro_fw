/*
 * Pyro flight computers — main() and the flight task [DD-073].
 *
 * main() brings the board up on core0 alone, then starts the scheduler. The
 * flight task runs the period's work, woken every LOOP_PERIOD_US by an alarm
 * on the hardware timer: not by the kernel's tick, which a flash operation
 * delays. A late period delays one wake, never the ones after it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/timer.h"
#include "FreeRTOS.h"
#include "task.h"

#include "hal.h"
#include "hal_storage.h"
#include "flash_op.h"
#include "rtos_tasks.h"
#include "pin_store.h"
#include "board_if.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "http_server.h"
#include "tusb.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/usb.h"

/* Each period takes the MS5607 conversion the last one commanded and
 * commands the next [DD-051]; the period is loop_period.h's [DD-065]. */
#include "loop_period.h"

/* Twice the board's declared worst case, so a board that exceeds its own
 * budget shows up in loop_overruns rather than being reset for it. */
#ifndef PYRO_LOOP_WORST_MS
#error "PYRO_LOOP_WORST_MS not defined - boards/<name>/board.cmake must set it"
#endif
#define WATCHDOG_MS (2u * PYRO_LOOP_WORST_MS)

/* High-water marks, reported by /api/status, so PYRO_LOOP_WORST_MS can be set
 * from measurement. loop_overruns above zero means a period's work did not
 * finish before the next period began; stage_max_us says which stage to look
 * at. Stage 1 was the USB and network pass, now the net task's; stage 8 is
 * the time the flight task spends waiting for its next period. */
#define STAGE_COUNT 9
#define STAGE_SLACK 8

volatile uint32_t loop_count;
volatile uint32_t loop_max_us;      /* longest period of work, the wait excluded */
volatile uint32_t loop_overruns;    /* periods whose wake came after the next one */
volatile uint32_t loop_late_max_us; /* worst delay from the alarm to the wake    */
volatile uint32_t stage_max_us[STAGE_COUNT];

static uint32_t stage_mark_us;
static uint8_t stage_cur;

/* Watchdog scratch 0..3 survive a reset and neither the SDK nor the
 * bootloader touches them, so after a hang the next boot can name the call
 * that stopped returning. */
static inline void stage_enter(uint8_t n, uint32_t now) {
    uint32_t t = time_us_32();
    uint32_t d = t - stage_mark_us;
    if (stage_cur < STAGE_COUNT && d > stage_max_us[stage_cur])
        stage_max_us[stage_cur] = d;
    stage_mark_us = t;
    stage_cur = (n < STAGE_COUNT) ? n : (uint8_t)(STAGE_COUNT - 1);
    watchdog_hw->scratch[0] = 0x53540000u | (uint32_t)n;
    watchdog_hw->scratch[1] = now;
}

#define STAGE(n) stage_enter((n), now)

/* Sub-steps, where a stage number is too coarse. Same encoding as
 * stage_enter, numbered above the stage range so the two cannot be confused,
 * and without stage_mark_us so a crumb does not distort a high-water mark.
 * The map:
 *
 *   60-61  around lua_core1_start()          (lua_app.c)
 *   80-82  an HTTP unit in the net task      (http_server.c)
 *   95-98  around one sector erase / program (littlefs_driver.c)
 *
 * flash_op_crumb() is the same store, callable from those files. 201-203 are
 * an RTOS assertion, a stack overflow and a panic (rtos_tasks.c), with the
 * detail in scratch[1]; 100-109 the boot, in main(). */

#if PYRO_HAS_LUA
#include "lua_app.h"
#endif

/* Network diagnostic counters (defined in net_glue.c / http_server.c) */
extern volatile uint32_t net_rx_count;
extern volatile uint32_t net_rx_drop;
extern volatile uint32_t net_tx_fail;
extern volatile uint32_t net_tx_ok;
volatile uint32_t net_http_accept;
volatile uint32_t net_http_err;
volatile uint32_t net_conn_full;

volatile device_status_t g_status = {0};

/* How the last boot ended, read before anything here restamps the scratch
 * registers or re-arms the watchdog; /api/status reports it. */
volatile bool boot_prev_watchdog;
volatile int32_t boot_prev_stage = -1;
volatile uint32_t boot_prev_stage_ms;

static void note_last_boot(void) {
    boot_prev_watchdog = watchdog_enable_caused_reboot();
    uint32_t st = watchdog_hw->scratch[0];
    if ((st & 0xffff0000u) == 0x53540000u) {
        boot_prev_stage = (int32_t)(st & 0xffffu);
        boot_prev_stage_ms = watchdog_hw->scratch[1];
    }
}

/* [USB-01] A host sends a start-of-frame every millisecond while it is awake,
 * and nothing else does, so a frame number that moves proves a PC and one that
 * stands still proves nothing: a charger, a sleeping host and no cable look
 * alike. VBUS cannot tell them apart either -- on these boards it reaches only
 * the charger IC, and TinyUSB overrides the SIE's detect bit. Every error here
 * is towards "not attached", which leaves launch detection on. */
#define USB_HOST_QUIET_MS 100u

static bool usb_host_active(uint32_t now) {
    static uint16_t frame;
    static uint32_t moved_ms;
    static bool moved;
    uint16_t f = (uint16_t)(usb_hw->sof_rd & USB_SOF_RD_BITS);
    if (f != frame) {
        frame = f;
        moved_ms = now;
        moved = true;
    }
    return moved && now - moved_ms < USB_HOST_QUIET_MS;
}

static void update_status(flight_context_t *ctx, uint32_t now) {
    g_status.state = ctx->current_state;
    g_status.altitude_cm = ctx->altitude_cm;
    g_status.max_altitude_cm = ctx->max_altitude_cm;
    g_status.vertical_speed_cms = ctx->speed_cms;
    g_status.pressure_pa = ctx->pressure_pa;
    g_status.pyro1_fired = ctx->fire.channel[0].fired;
    g_status.pyro2_fired = ctx->fire.channel[1].fired;
    g_status.pyros_armed = ctx->pyros_armed;
    g_status.pyro1_continuity = ctx->channel_ready[0];
    g_status.pyro2_continuity = ctx->channel_ready[1];
    g_status.pyro1_adc = ctx->channel_adc[0];
    g_status.pyro2_adc = ctx->channel_adc[1];
    g_status.under_thrust = ctx->under_thrust;
    g_status.flight_time_ms = flight_elapsed_ms(ctx, now);
    g_status.pyro1_mode = ctx->config.pyro1_mode;
    g_status.pyro1_value = ctx->config.pyro1_value;
    g_status.pyro2_mode = ctx->config.pyro2_mode;
    g_status.pyro2_value = ctx->config.pyro2_value;
    g_status.units = ctx->config.units;
    memcpy((char *)g_status.rocket_id, ctx->config.id, 9);
    memcpy((char *)g_status.rocket_name, ctx->config.name, 9);
}

/* ── The period ───────────────────────────────────────────────────────
 *
 * The alarm re-arms itself on a fixed grid. A wake the flight task has not
 * reached by the next alarm is counted as an overrun, and the grid skips
 * forward rather than catching up: adding periods to a missed target would
 * compress the periods after it. */
static TaskHandle_t flight_h;
static int period_alarm = -1;
static volatile uint64_t period_target_us;
static volatile uint32_t period_fires;

static void period_isr(uint alarm) {
    uint64_t now = time_us_64();
    uint64_t next = period_target_us + LOOP_PERIOD_US;
    if ((int64_t)(next - now) < 200)
        next = now + LOOP_PERIOD_US;
    period_target_us = next;
    hardware_alarm_set_target(alarm, from_us_since_boot(next));
    period_fires++;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(flight_h, &woken);
    portYIELD_FROM_ISR(woken);
}

/* From the flight task, so the alarm's interrupt belongs to core0. */
static void period_start(void) {
    flight_h = xTaskGetCurrentTaskHandle();
    period_alarm = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback((uint)period_alarm, period_isr);
    period_target_us = time_us_64() + LOOP_PERIOD_US;
    hardware_alarm_set_target((uint)period_alarm, from_us_since_boot(period_target_us));
}

void flight_task(void *arg) {
    flight_context_t *ctx = (flight_context_t *)arg;
    bool reset_armed = false; /* see the pending_reset handling below */

    /* After USB, lwIP and the filesystem, which are slow enough to trip it.
     * The safe-boot latch in lua_app.c cannot fire without this. */
    watchdog_enable(WATCHDOG_MS, true);
    period_start();
    stage_mark_us = time_us_32();
    stage_cur = STAGE_SLACK;
    uint32_t fires_seen = 0;

    for (;;) {
        /* The flight task's only blocking call [plan 2, section 8]. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t fires = period_fires;
        if (fires - fires_seen > 1u)
            loop_overruns += fires - fires_seen - 1u;
        fires_seen = fires;
        uint32_t late = (uint32_t)(time_us_64() - (period_target_us - LOOP_PERIOD_US));
        if (late < LOOP_PERIOD_US && late > loop_late_max_us)
            loop_late_max_us = late;

        uint32_t now = hal_time_ms();
        uint32_t iter_t0 = time_us_32();

        /* watchdog_reboot() works by loading a short timeout and letting it
         * expire, so feeding afterwards cancels the reboot. On the ground a
         * storage task that has stopped checking in is not fed for: the
         * board resets. In flight it is counted, and the flight goes on. */
        if (!reset_armed && (storage_alive(now) || hal_log_active())) {
            watchdog_update();
        }

        /* First: the MS5607 is commanded at a steady offset from the period,
         * so the conversion's one-shot has read it before the next period
         * comes to take it. */
        STAGE(2);
        hal_tasks_tick(now);

        extern volatile uint8_t pending_reset;
        if (pending_reset == 1)
            rom_reset_usb_boot(0, 0); /* never returns; scratch does not survive it */

        /* Arm once: pending_reset stays set, and re-arming every period would
         * reload the countdown faster than it can expire. The net task keeps
         * running meanwhile, so the HTTP response flushes first. An OTA
         * reboots through here too, once its reply is with lwIP. */
        if (pending_reset == 2 && !reset_armed) {
            reset_armed = true;
#if PYRO_HAS_LUA
            lua_app_restart_commanded();
#endif
            watchdog_reboot(0, 0, 100);
        }

        /* Changes another task handed over: config, test mode, a buzzer
         * audition. Applied here, before the flight software reads them. */
        STAGE(3);
        flight_call_service();
        flight_set_usb_attached(ctx, usb_host_active(now), now);
        ctx->current_state = dispatch_state(ctx, now);

        /* Outputs (telemetry, pyro update) */
        STAGE(4);
        flight_update_outputs(ctx, now);
        STAGE(5);
        update_status(ctx, now);

#if PYRO_HAS_LUA
        STAGE(6);
        lua_app_service(ctx, now);
        lua_app_dispatch();
#endif

        /* The flight log's ring and the pad marker are the storage task's to
         * write: wake it. Never waits. */
        STAGE(7);
        rtos_notify_storage();

        STAGE(STAGE_SLACK);
        uint32_t work_us = time_us_32() - iter_t0;
        if (work_us > loop_max_us)
            loop_max_us = work_us;
        loop_count++;
    }
}

static flight_context_t ctx;

#define BOOT(n) (watchdog_hw->scratch[0] = 0x53540000u | (100u + (n)))

int main() {
    note_last_boot();
    BOOT(0);
    hal_platform_init();
    BOOT(1);

    flight_init(&ctx);
    BOOT(2);

    /* Before lua_app_init(), which configures Lua from the assignment. A
     * rejected pins.ini falls back to the board defaults and says so on
     * /api/status. */
    pin_store_load(NULL, 0);

    /* The single pass that gives every pad one owner, then the flight
     * software spending its claims. Both before the scheduler starts, and in
     * this order: whatever the flight software claims here,
     * lua_plat_configure() can no longer publish, and the pads it could not
     * claim are Lua's. See pad_claim.h. */
    pin_store_claim_pads();
    hal_pyro_claim_channels(pin_store_pyro_pads);

    /* Move the buzzer to whatever pad the assignment names. After the claim
     * pass, because that is what reserved the pad; a saved pins.ini reports
     * reboot_required, so this is the only place it needs applying. On MK1A
     * this is how the board gets a buzzer at all. */
    board_buzzer_set_pin(pin_assign_buzzer_pin(pin_store_current()));

    /* The ground test switch's pads, claimed by the same pass; read from the
     * first period, which is where the power-up settle watches it. */
    const pin_assign_t *pins = pin_store_current();
    hal_ground_test_configure(pins->gt_wiring, pins->gt_pin, pins->gt_drive_pin);

#if PYRO_HAS_LUA
    /* Configures Lua from the assignment and reads the script; the script
     * starts two seconds into the flight task's periods. See lua_core1.h. */
    lua_app_init(&ctx.config);
#endif

    BOOT(3);
    rtos_start(&ctx); /* never returns */
    return 0;
}
