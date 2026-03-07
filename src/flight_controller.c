/*
 * Pyro MK1B Flight Controller — main loop.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#include "flight_states.h"
#include "device_status.h"
#include "pyro.h"
#include "buzzer.h"
#include "version.h"

volatile device_status_t g_status = {0};

/* Network (defined in net_glue.c) */
void net_mac_init(void);
void net_service(void);

int main() {
    board_init();
    net_mac_init();
    tud_init(BOARD_TUD_RHPORT);
    stdio_init_all();

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    uart_puts(uart0, "UART initialized\r\n");
    uart_puts(uart0, "Pyro MK1B v" FW_VERSION " (" FW_BUILD_DATE ")\r\n");

    flight_context_t ctx = {0};
    ctx.config = (config_t){"PYRO001", "PYRO001", 1, 300, 1, 150};
    ctx.current_state = BOOT_FILESYSTEM;

    static uint32_t last_hb = 0;

    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        tud_task();
        net_service();

        /* Handle deferred picotool reset */
        extern volatile uint8_t pending_reset;
        if (pending_reset == 1) rom_reset_usb_boot(0, 0);
        if (pending_reset == 2) watchdog_reboot(0, 0, 100);

        /* Unified state machine */
        ctx.current_state = dispatch_state(&ctx, now);

        /* Update pyro */
        pyro_update(now);
        buzzer_update(now);

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

        /* $PYRO telemetry: 10Hz during ASCENT/DESCENT, 1Hz otherwise, skip during boot */
        if (ctx.current_state >= PAD_IDLE) {
            uint32_t telem_interval = (ctx.current_state == ASCENT || ctx.current_state == DESCENT) ? 100 : 1000;
            if (now - ctx.last_telemetry >= telem_interval) {
                uint32_t flight_time = (ctx.current_state != PAD_IDLE) ? (now - ctx.launch_time) : 0;
                send_telemetry(&ctx, flight_time, ctx.last_altitude, ctx.current_state);
                ctx.last_telemetry = now;
            }
        }

        /* Heartbeat */
        if (now - last_hb >= 1000) {
            last_hb = now;
            char hb[48];
            snprintf(hb, sizeof(hb), "[%lu] alive pa=%ld\r\n",
                     (unsigned long)now, (long)g_status.pressure_pa);
            uart_puts(uart0, hb);
        }
    }

    return 0;
}
