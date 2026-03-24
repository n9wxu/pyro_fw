/*
 * Pyro MK1B Flight Controller — hardware main loop.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#include "hal.h"
#include "flight_states.h"
#include "device_status.h"
#include "buzzer.h"
#include "version.h"

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
    flight_init(&ctx); /* [v2] detect_boot_init() → hal_config_load() */

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

        /* Flight software — batch mode when FIFO ready, polled fallback */
        hal_pressure_batch_t batch;
        if (hal_pressure_fifo_get(&batch)) {
            flight_process_samples(&ctx, &batch);
            hal_pressure_fifo_release();
        } else {
            ctx.current_state = dispatch_state(&ctx, now);
        }

        /* Outputs (telemetry, pyro update) */
        flight_update_outputs(&ctx, now);
        /* csv_flush_safe/step removed: hal_log async task owns the flight record [v2-9] */
        update_status(&ctx, now);

        /* [v2, PWR-SLEEP-01] Sleep until the next pressure batch, serial
         * input, or timer event.  On RP2040 this calls __wfe(); the USB
         * and network service interrupts wake the CPU as needed.
         * When Core1 pressure FIFO is implemented (Task 7) Core1 will
         * send a SEV on batch-ready, keeping the CPU in WFE ~97% of
         * the time. */
        hal_sleep_until_event();
    }
}
