/*
 * Pyro MK1B Flight Controller — hardware main loop.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "ms5607_driver.h"

#include "hal.h"
#include "flash_window.h"
#include "pin_store.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "tusb.h"
#include "hardware/structs/watchdog.h"

/* The period is one MS5607 conversion phase: the pressure task is a
 * three-phase state machine clocked at MS5607_CONV_MS, so one iteration
 * advances it by one phase. */
#define LOOP_PERIOD_MS MS5607_CONV_MS
#define LOOP_PERIOD_US (LOOP_PERIOD_MS * 1000u)

/* Twice the board's declared worst case, so a board that exceeds its own
 * budget shows up in loop_overruns rather than being reset for it. */
#ifndef PYRO_LOOP_WORST_MS
#error "PYRO_LOOP_WORST_MS not defined - boards/<name>/board.cmake must set it"
#endif
#define WATCHDOG_MS (2u * PYRO_LOOP_WORST_MS)

/* High-water marks, reported by /api/status, so PYRO_LOOP_WORST_MS can be set
 * from measurement. loop_overruns above zero means the budget is optimistic
 * or the period is too short; stage_max_us says which stage to look at. */
#define STAGE_COUNT 9
#define STAGE_SLACK 8

volatile uint32_t loop_count;
volatile uint32_t loop_max_us;      /* longest iteration of work, slack excluded */
volatile uint32_t loop_overruns;    /* iterations that missed the deadline       */
volatile uint32_t loop_late_max_us; /* worst overshoot past the deadline         */
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

/* Sub-steps inside a stage, where a stage number is too coarse: one stage
 * covers several calls that can each stop returning. Same encoding as
 * stage_enter, numbered above the stage range so the two cannot be confused,
 * and without stage_mark_us so a crumb does not distort a high-water mark.
 * The map:
 *
 *   70-74  core0 in the flash window (see below)
 *   60-61  around lua_core1_start()          (lua_app.c)
 *   95-98  around one sector erase / program (littlefs_driver.c)
 *
 * flash_window_crumb() is the same store, callable from those files. */
#define CRUMB(n) (watchdog_hw->scratch[0] = 0x53540000u | (uint32_t)(n))

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

void net_mdns_poll(void);
void net_service(void); /* net_glue.c; serviced again in the loop's slack */

static void update_status(flight_context_t *ctx, uint32_t now) {
    g_status.state = ctx->current_state;
    g_status.altitude_cm = ctx->last_altitude;
    g_status.max_altitude_cm = ctx->max_altitude;
    g_status.vertical_speed_cms = ctx->vertical_speed_cms;
    g_status.pressure_pa = ctx->filtered_pressure;
    g_status.pyro1_fired = ctx->pyro1_fired;
    g_status.pyro2_fired = ctx->pyro2_fired;
    g_status.pyros_armed = ctx->pyros_armed;
    g_status.pyro1_continuity = ctx->pyro1_continuity_good;
    g_status.pyro2_continuity = ctx->pyro2_continuity_good;
    g_status.pyro1_adc = ctx->pyro1_adc;
    g_status.pyro2_adc = ctx->pyro2_adc;
    g_status.under_thrust = ctx->under_thrust;
    g_status.flight_time_ms = (ctx->launch_time > 0) ? (now - ctx->launch_time) : 0;
    g_status.pyro1_mode = ctx->config.pyro1_mode;
    g_status.pyro1_value = ctx->config.pyro1_value;
    g_status.pyro2_mode = ctx->config.pyro2_mode;
    g_status.pyro2_value = ctx->config.pyro2_value;
    g_status.units = ctx->config.units;
    memcpy((char *)g_status.rocket_id, ctx->config.id, 9);
    memcpy((char *)g_status.rocket_name, ctx->config.name, 9);
}

int main() {
    hal_platform_init();

    flight_context_t ctx;
    flight_init(&ctx);

    /* Before lua_app_init(), which configures Lua from the assignment. A
     * rejected pins.ini falls back to the migrated legacy one and says so on
     * /api/status. */
    pin_store_load(&ctx.config, NULL, 0);

    /* The single pass that gives every pad one owner, then the flight
     * software spending its claims. Both before core1 exists, and in this
     * order: whatever the flight software claims here, lua_plat_configure()
     * can no longer publish, and the pads it could not claim are Lua's. See
     * pad_claim.h. */
    pin_store_claim_pads();
    hal_pyro_claim_channels(pin_store_pyro_pads);

#if PYRO_HAS_LUA
    /* Core1 is launched here, once, after the filesystem is mounted and the
     * config is loaded, and before the flight loop. There is deliberately no
     * relaunch path: see src/lua/lua_core1.h. */
    lua_app_init(&ctx.config);
#endif

    bool reset_armed = false; /* see the pending_reset handling below */

    /* After USB enumeration, lwIP and the filesystem mount, which are slow
     * enough to trip a watchdog armed in hal_platform_init(). The safe-boot
     * latch in lua_app.c cannot fire without this. */
    watchdog_enable(WATCHDOG_MS, true);

    /* Prime both, or the first iteration measures time since power-on. */
    absolute_time_t deadline = make_timeout_time_us(LOOP_PERIOD_US);
    stage_mark_us = time_us_32();
    stage_cur = STAGE_SLACK;

    while (1) {
        uint32_t now = hal_time_ms();
        uint32_t iter_t0 = time_us_32();

        /* watchdog_reboot() works by loading a short timeout and letting it
         * expire, so feeding afterwards cancels the reboot.
         *
         * The pyro arm window disables the watchdog when it finishes, hence
         * the re-arm. */
        if (!reset_armed) {
            if (!(watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS)) {
                watchdog_enable(WATCHDOG_MS, true);
            }
            watchdog_update();
        }

        /* Platform services */
        STAGE(1);
        hal_platform_service();

        extern volatile uint8_t pending_reset;
        if (pending_reset == 1)
            rom_reset_usb_boot(0, 0); /* never returns */

        /* Arm once: pending_reset stays set, and re-arming every iteration
         * would reload the countdown faster than it can expire. The loop
         * keeps running so the in-flight HTTP response flushes first.
         * pfb_perform_update() reboots through here too. */
        if (pending_reset == 2 && !reset_armed) {
            reset_armed = true;
            watchdog_reboot(0, 0, 100);
        }

        /* Advance async HAL state machines (pressure, buzzer, log flush) */
        STAGE(2);
        hal_tasks_tick(now);

        /* Flight software — single code path via pressure_processing ring.
         * dispatch_state() internally reads altitude samples via pp_read(). */
        STAGE(3);
        ctx.current_state = dispatch_state(&ctx, now);

        /* Outputs (telemetry, pyro update) */
        STAGE(4);
        flight_update_outputs(&ctx, now);
        STAGE(5);
        update_status(&ctx, now);

#if PYRO_HAS_LUA
        STAGE(6);
        lua_app_service(&ctx, now);
#endif

        /* Core1 is idle here because core0 handed it no work since the
         * previous period's dispatch, and sized that grant to expire before
         * this line. A core1 still executing -- an overrunning unit, or its
         * unbounded startup -- leaves the window shut for the period rather
         * than stalling core0. See flash_window.h. */
        STAGE(7);
        CRUMB(70);
        bool window = lua_core1_flash_ok();
        if (window) {
            CRUMB(71);
            flash_window_open();
            hal_flash_service(now);
            CRUMB(72);
        } else {
            CRUMB(74);
            flash_window_skipped();
        }

        /* Slack from here, spent servicing USB and lwIP rather than
         * sleeping: throttling those to the period rate costs HTTP and OTA
         * throughput.
         *
         * A live hold gives core1 nothing for the whole period. One sector
         * erase takes tens of milliseconds, so an upload cannot run
         * concurrently with core1 on any schedule; core0 makes that trade
         * explicitly and briefly. */
        STAGE(STAGE_SLACK);
        uint32_t work_us = time_us_32() - iter_t0;
        if (work_us > loop_max_us)
            loop_max_us = work_us;
        loop_count++;

#if PYRO_HAS_LUA
        if (window && !flash_window_holding(now)) {
            flash_window_close();
            lua_app_dispatch(absolute_time_diff_us(get_absolute_time(), deadline));
        }
#endif

        /* Tested before the slack loop, which by definition exits at the
         * deadline and would make every iteration an overrun. */
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            loop_overruns++;
        } else {
            while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
                tud_task();
                /* Core0 hands out no more work until the next period, so a
                 * core1 observed idle here stays idle for the rest of the
                 * slack. Without this, a config save waits out a 250 ms
                 * tcp_fasttmr before lwIP redelivers it. */
                if (!flash_window_is_open() && lua_core1_flash_ok()) {
                    flash_window_open();
                }
                net_service();
            }
        }

        /* Unconditional: a flash write from a USB callback or an interrupt
         * must fail rather than land while core1 is running. */
        flash_window_close();

        /* Counts both causes: work that did not fit, and a final USB/lwIP
         * pass that overshot. */
        int64_t late_us = absolute_time_diff_us(deadline, get_absolute_time());
        if (late_us > 0 && (uint32_t)late_us > loop_late_max_us)
            loop_late_max_us = (uint32_t)late_us;

        /* Skip rather than catch up: adding a period to the previous
         * deadline compresses the iterations after an overrun. */
        deadline = make_timeout_time_us(LOOP_PERIOD_US);
    }
}
