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

#include "hal.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "tusb.h"
#include "hardware/structs/watchdog.h"

/* Long enough that no normal iteration can trip it -- an OTA sector write,
 * a log flush and a full lwIP service all fit inside it with room to spare --
 * and short enough that a wedged board comes back while someone is still
 * standing at the pad. */
#define WATCHDOG_BOOT_MS 4000

/* Main-loop breadcrumb.
 *
 * scratch[0] is the stage core0 was last in, scratch[1] the millisecond it
 * entered it. Both survive a watchdog reset, so after a hang the next boot
 * can say exactly which call stopped returning instead of leaving it to be
 * inferred. Two register writes per stage; scratch 0..3 are untouched by the
 * SDK and the bootloader. */
#define STAGE(n)                                                                                                       \
    do {                                                                                                               \
        watchdog_hw->scratch[0] = 0x53540000u | (n);                                                                   \
        watchdog_hw->scratch[1] = now;                                                                                 \
    } while (0)

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
    watchdog_enable(WATCHDOG_BOOT_MS, true);

    while (1) {
        uint32_t now = hal_time_ms();

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
                watchdog_enable(WATCHDOG_BOOT_MS, true);
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
        STAGE(7);
#endif
    }
}
