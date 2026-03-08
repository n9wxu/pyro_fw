/*
 * Pyro MK1B Flight Controller — hardware main loop.
 * This is the only file that includes platform-specific headers.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "hal.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "version.h"

volatile device_status_t g_status = {0};

/* Network (defined in net_glue.c) */
void net_mdns_poll(void);

int main() {
    hal_platform_init();

    flight_context_t ctx = {0};
    /* Defaults — overridden by config.ini parser in boot_filesystem */
    strncpy(ctx.config.id, "PYRO001", 8);
    strncpy(ctx.config.name, "PYRO001", 8);
    ctx.config.pyro1_mode = PYRO_MODE_DELAY;
    ctx.config.pyro1_value = 0;
    ctx.config.pyro2_mode = PYRO_MODE_AGL;
    ctx.config.pyro2_value = 300;
    ctx.config.units = 1;
    ctx.current_state = BOOT_FILESYSTEM;

    while (1) {
        uint32_t now = hal_time_ms();

        hal_platform_service();

        /* Handle deferred picotool reset */
        extern volatile uint8_t pending_reset;
        if (pending_reset == 1) rom_reset_usb_boot(0, 0);
        if (pending_reset == 2) watchdog_reboot(0, 0, 100);

        /* Unified state machine */
        ctx.current_state = dispatch_state(&ctx, now);

        hal_pyro_update(now);
        buzzer_update(now);

        /* mDNS polling (hardware-specific) */
        if (ctx.current_state == BOOT_MDNS)
            net_mdns_poll();

        /* Update shared status for HTTP dashboard */
        g_status.state = ctx.current_state;
        g_status.altitude_cm = ctx.last_altitude;
        g_status.max_altitude_cm = ctx.max_altitude;
        g_status.vertical_speed_cms = ctx.vertical_speed_cms;
        g_status.pressure_pa = ctx.filtered_pressure;
        g_status.pyro1_fired = ctx.pyro1_fired;
        g_status.pyro2_fired = ctx.pyro2_fired;
        g_status.pyros_armed = ctx.pyros_armed;
        g_status.pyro1_continuity = ctx.pyro1_continuity_good;
        g_status.pyro2_continuity = ctx.pyro2_continuity_good;
        g_status.pyro1_adc = ctx.pyro1_adc;
        g_status.pyro2_adc = ctx.pyro2_adc;
        g_status.under_thrust = ctx.under_thrust;
        g_status.flight_time_ms = (ctx.launch_time > 0) ? (now - ctx.launch_time) : 0;
        g_status.pyro1_mode = ctx.config.pyro1_mode;
        g_status.pyro1_value = ctx.config.pyro1_value;
        g_status.pyro2_mode = ctx.config.pyro2_mode;
        g_status.pyro2_value = ctx.config.pyro2_value;
        g_status.units = ctx.config.units;
        memcpy((char*)g_status.rocket_id, ctx.config.id, 9);
        memcpy((char*)g_status.rocket_name, ctx.config.name, 9);

        /* $PYRO telemetry */
        if (ctx.current_state >= PAD_IDLE) {
            uint32_t telem_interval = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
            if (now - ctx.last_telemetry >= telem_interval) {
                uint32_t flight_time = (ctx.current_state != PAD_IDLE) ? (now - ctx.launch_time) : 0;
                send_telemetry(&ctx, flight_time, ctx.last_altitude, ctx.current_state);
                ctx.last_telemetry = now;
            }
        }
    }

    return 0;
}
