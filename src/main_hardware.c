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
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "tusb.h"
#include "hardware/structs/watchdog.h"

/* ── Main-loop period ─────────────────────────────────────────────
 *
 * The loop runs on a fixed period instead of free-running. Three things
 * need one to exist:
 *
 *   - the watchdog timeout, which is now derived from a board's declared
 *     worst case rather than being a round number nobody can justify;
 *   - a board replacing a blocking settle (sleep_ms) with "sample on the
 *     next tick", which is only expressible once the tick is a known
 *     interval;
 *   - work handed to a second core, which can then be sized as "whatever
 *     is left before the deadline".
 *
 * The period is one MS5607 conversion phase, which is not arbitrary: the
 * pressure task is a three-phase state machine clocked at MS5607_CONV_MS,
 * so one iteration advances it by exactly one phase.
 *
 * Slack is spent servicing USB and lwIP rather than sleeping. That keeps
 * network throughput at what the free-running loop used to deliver, and it
 * is why this change is behaviour-neutral for a board that does not yet
 * care about the period. */
#define LOOP_PERIOD_MS MS5607_CONV_MS
#define LOOP_PERIOD_US (LOOP_PERIOD_MS * 1000u)

/* Watchdog: twice the board's declared worst-case iteration, from
 * boards/<name>/board.cmake. A board that blocks longer than its own budget
 * is reported by loop_overruns below rather than being reset for it, so the
 * number can be corrected from evidence instead of by guessing lower. */
#ifndef PYRO_LOOP_WORST_MS
#error "PYRO_LOOP_WORST_MS not defined - boards/<name>/board.cmake must set it"
#endif
#define WATCHDOG_MS (2u * PYRO_LOOP_WORST_MS)

/* ── Loop instrumentation ─────────────────────────────────────────
 *
 * High-water marks, not averages: a budget is a statement about the worst
 * iteration, and nothing here measured that before. Reported by
 * /api/status, so PYRO_LOOP_WORST_MS can be set from evidence.
 *
 * loop_overruns > 0 means the declared budget is optimistic or the period
 * is too short; stage_max_us says which stage to look at. */
#define STAGE_COUNT 9
#define STAGE_SLACK 8

volatile uint32_t loop_count;
volatile uint32_t loop_max_us;      /* longest iteration of work, slack excluded */
volatile uint32_t loop_overruns;    /* iterations that missed the deadline       */
volatile uint32_t loop_late_max_us; /* worst overshoot past the deadline         */
volatile uint32_t stage_max_us[STAGE_COUNT];

static uint32_t stage_mark_us;
static uint8_t stage_cur;

/* Main-loop breadcrumb.
 *
 * scratch[0] is the stage core0 was last in, scratch[1] the millisecond it
 * entered it. Both survive a watchdog reset, so after a hang the next boot
 * can say exactly which call stopped returning instead of leaving it to be
 * inferred. scratch 0..3 are untouched by the SDK and the bootloader.
 *
 * Also where each stage is timed, because the breadcrumb already marks every
 * boundary the timing would need: a stage ends exactly where the next one is
 * recorded, so the high-water marks cost one timer read per stage and no new
 * call sites. */
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

/* A breadcrumb WITHOUT the timing side effects, for sub-steps inside a stage.
 *
 * Same encoding as stage_enter's, so the safe-boot latch decodes one with no
 * new plumbing and reports it as "stage N"; the numbers sit above the stage
 * range so the two cannot be confused. It does not touch stage_mark_us, so
 * adding one does not distort a stage's high-water mark.
 *
 * These are here because a stage number alone was not enough to find the
 * deadlock. "Died in stage 6" covered the launch, the log flush and the
 * dispatch; the crumbs narrowed the same failure to a single line inside
 * flash_range_program(), which is what identified it. The map:
 *
 *   70-74  core0 in the flash window (see below)
 *   60-61  around lua_core1_start()          (lua_app.c)
 *   90-93  around one log flush              (lua_app.c)
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

#if PYRO_HAS_LUA
    /* Core1 is launched here, once, after the filesystem is mounted and the
     * config is loaded, and before the flight loop. There is deliberately no
     * relaunch path: see src/lua/lua_core1.h. */
    lua_app_init(&ctx.config);
#endif

    bool reset_armed = false; /* see the pending_reset handling below */

    /* Boot watchdog.
     *
     * hal_platform_init() used to arm one with a 1 ms timeout, which caused
     * boot loops, and the fix at the time was to remove it entirely. That
     * left a hang in the main loop as a hard brick recoverable only with the
     * BOOTSEL button -- which is what a bad Lua program on core1 produced on
     * the bench.
     *
     * A generous timeout gets the useful half back. It is armed here rather
     * than in platform init so everything slow (USB enumeration, lwIP, the
     * filesystem mount) is already finished, and it is what makes the
     * safe-boot latch in lua_app.c able to fire at all. */
    watchdog_enable(WATCHDOG_MS, true);

    /* Prime the pacing and the stage timer together, so the first iteration
     * measures a real interval rather than time since power-on. */
    absolute_time_t deadline = make_timeout_time_us(LOOP_PERIOD_US);
    stage_mark_us = time_us_32();
    stage_cur = STAGE_SLACK;

    while (1) {
        uint32_t now = hal_time_ms();
        uint32_t iter_t0 = time_us_32();

        /* Feed the watchdog -- but NOT once a deliberate reset is armed.
         *
         * watchdog_reboot() works by loading a short timeout and letting it
         * expire. Feeding the watchdog afterwards reloads that countdown, so
         * the reset never lands. That is the same trap the pending_reset
         * comment below describes, and re-arming here walked straight back
         * into it: /api/reboot answered "Rebooting" and the board carried on
         * running, which also silently breaks OTA.
         *
         * The pyro arm window enables the watchdog with its own short timeout
         * and disables it afterwards, so re-arm if it went away rather than
         * running unprotected from then on. */
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

        /* watchdog_reboot() ARMS the watchdog with a timeout; it does not
         * schedule a one-shot. pending_reset stays set, so calling it every
         * iteration reloaded the 100 ms countdown faster than it could ever
         * expire and the device never rebooted -- which also silently broke
         * OTA, since pfb_perform_update() reboots through this same path.
         * Arm exactly once, then let the loop keep servicing USB and lwIP so
         * the in-flight HTTP response still flushes before the reset lands. */
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

        /* ── The flash window ─────────────────────────────────────
         *
         * The one point in the period where this firmware erases or programs
         * flash, and it is a point core0 CHOOSES rather than one it waits
         * for. Core1 is idle here because core0 has not handed it work since
         * the previous period's dispatch, and that grant was sized to expire
         * before this line -- not because core1 was asked to stop and
         * answered.
         *
         * Everything above queued its bytes in RAM. hal_flash_service()
         * writes the flight log, lua_app_flash_service() the script log, and
         * the slack loop below writes uploads and OTA sectors when a hold is
         * live. Outside the window every one of those fails cleanly and
         * retries a period later; nothing anywhere spins.
         *
         * If core1 is still executing -- an overrunning unit, or its
         * unbounded startup -- the window does not open at all. That is a
         * skipped period, counted and reported, not a stall: core0 carries
         * straight on and lua_core1_service() above kills a core1 that keeps
         * doing it. */
        STAGE(7);
        CRUMB(70);
        bool window = lua_core1_flash_ok();
        if (window) {
            CRUMB(71);
            flash_window_open();
            hal_flash_service(now);
            CRUMB(72);
#if PYRO_HAS_LUA
            lua_app_flash_service(now);
#endif
            CRUMB(73);
        } else {
            CRUMB(74);
            flash_window_skipped();
        }

        /* ── Pace to the period ──
         *
         * Everything past here is slack. It is spent servicing USB and lwIP
         * rather than sleeping: those are what the free-running loop used to
         * call as fast as it could, and throttling them to the period rate
         * would cost HTTP and OTA throughput for nothing.
         *
         * Two things can happen with core1 here, and the choice is the whole
         * flash policy:
         *
         *   - no hold: the window closes, core1 gets the slack as a bounded
         *     unit, and an HTTP handler that wants flash pushes back on its
         *     TCP connection and arms a hold for next period.
         *   - hold live: core1 gets NOTHING this period. It stays parked in
         *     RAM, the window stays open across the whole slack, and the
         *     upload or OTA writes as many sectors as it can. One sector
         *     erase is tens of milliseconds, which is several periods' worth
         *     of Lua -- there is no version of this where an upload runs
         *     concurrently with core1, so core0 makes the trade explicitly
         *     and briefly rather than colliding with it. */
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

        /* An overrun is "the work left no slack at all", tested BEFORE the
         * slack loop. Testing after would count every iteration, because the
         * slack loop by definition exits at the deadline. */
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            loop_overruns++;
        } else {
            while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
                tud_task();
                /* Reopen the window the moment core1 finishes early.
                 *
                 * Core0 hands out no more work until the next period, so an
                 * idle core1 observed here STAYS idle for the rest of the
                 * slack -- which makes the rest of the slack a genuine
                 * window, not a guess. A tick() that returns in 200 us
                 * leaves milliseconds core0 can write flash in, and this is
                 * what lets an upload or a config save land on its first
                 * packet rather than being handed back to lwIP and waiting
                 * out a 250 ms tcp_fasttmr before it is redelivered.
                 *
                 * Still a read and never a wait: if core1 is working, the
                 * window simply does not open this period. */
                if (!flash_window_is_open() && lua_core1_flash_ok()) {
                    flash_window_open();
                }
                net_service();
            }
        }

        /* Shut unconditionally. A flash write that arrives outside this loop
         * -- from a USB callback, an interrupt, anything that is not core0
         * here -- must fail rather than land while core1 is running. */
        flash_window_close();

        /* How late the next iteration actually starts, whatever the cause:
         * work that did not fit, or a final USB/lwIP pass that overshot. */
        int64_t late_us = absolute_time_diff_us(deadline, get_absolute_time());
        if (late_us > 0 && (uint32_t)late_us > loop_late_max_us)
            loop_late_max_us = (uint32_t)late_us;

        /* Skip, never catch up. Adding a period to the old deadline would
         * compress the iterations after an overrun and turn one late pass
         * into several. */
        deadline = make_timeout_time_us(LOOP_PERIOD_US);
    }
}
