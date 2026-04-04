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
#include "hardware/uart.h"

#include "hal.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "version.h"
#include "tusb.h"

/* Network diagnostic counters (defined in net_glue.c / http_server.c) */
extern volatile uint32_t net_rx_count;
extern volatile uint32_t net_rx_drop;
extern volatile uint32_t net_tx_fail;
extern volatile uint32_t net_tx_ok;
volatile uint32_t net_http_accept;
volatile uint32_t net_http_err;
volatile uint32_t net_conn_full;

/* Debug print helper — writes directly to uart0.
 * Safe at low frequency; avoids conflict with telemetry ring buffer
 * since debug prints are infrequent and blocking is brief. */
static void dbg(const char *msg) {
    uart_puts(uart0, msg);
}

static const char *state_label[] = {"BOOT_SETTLE", "BOOT_CONT", "BOOT_CAL", "PAD_IDLE", "ASCENT", "DESCENT", "LANDED"};

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
    dbg("\r\n\r\n=== PYRO MK1B BOOT ===\r\n");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "FW: %s  uptime: %lu ms\r\n", FW_VERSION, (unsigned long)hal_time_ms());
        dbg(buf);
    }

    dbg("flight_init()...\r\n");
    flight_context_t ctx;
    flight_init(&ctx); /* [v2] detect_boot_init() → hal_config_load() */
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "flight_init done. state=%s  t=%lu\r\n", state_label[ctx.current_state],
                 (unsigned long)hal_time_ms());
        dbg(buf);
    }

    uint32_t last_dbg_ms = 0;
    uint32_t last_net_ms = 0;
    flight_state_t prev_state = ctx.current_state;

    while (1) {
        uint32_t now = hal_time_ms();

        /* Platform services */
        hal_platform_service();

        extern volatile uint8_t pending_reset;
        if (pending_reset == 1)
            rom_reset_usb_boot(0, 0);
        if (pending_reset == 2)
            watchdog_reboot(0, 0, 100);

        /* Advance async HAL state machines (pressure, buzzer, log flush) */
        hal_tasks_tick(now);

        /* Flight software — single code path via pressure_processing ring.
         * dispatch_state() internally reads altitude samples via pp_read(). */
        ctx.current_state = dispatch_state(&ctx, now);

        /* Outputs (telemetry, pyro update) */
        flight_update_outputs(&ctx, now);
        /* csv_flush_safe/step removed: hal_log async task owns the flight record [v2-9] */
        update_status(&ctx, now);

        /* Debug: log state transitions immediately */
        if (ctx.current_state != prev_state) {
            char buf[64];
            snprintf(buf, sizeof(buf), "[%lu] STATE: %s -> %s\r\n", (unsigned long)now, state_label[prev_state],
                     state_label[ctx.current_state]);
            dbg(buf);
            prev_state = ctx.current_state;
        }

        /* Debug: periodic heartbeat every 2 seconds */
        if (now - last_dbg_ms >= 2000) {
            char buf[80];
            snprintf(buf, sizeof(buf), "[%lu] %s alt=%ld spd=%ld\r\n", (unsigned long)now,
                     state_label[ctx.current_state], (long)ctx.last_altitude, (long)ctx.vertical_speed_cms);
            dbg(buf);
            last_dbg_ms = now;
        }

        /* Network health: every 10 seconds */
        if (now - last_net_ms >= 10000) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[%lu] NET: tud=%d rx=%lu drop=%lu tx=%lu txf=%lu http=%lu\r\n",
                     (unsigned long)now, tud_ready() ? 1 : 0, (unsigned long)net_rx_count, (unsigned long)net_rx_drop,
                     (unsigned long)net_tx_ok, (unsigned long)net_tx_fail, (unsigned long)net_http_accept);
            dbg(buf);
            last_net_ms = now;
        }

        /* [v2.1.27] WFE sleep removed — suspected of starving USB NCM TX.
         * CPU busy-loops until root cause of txf accumulation is confirmed.
         * See STATUS.md network diagnostics section. */
    }
}
