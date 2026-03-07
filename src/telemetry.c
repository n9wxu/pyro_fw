/*
 * $PYRO NMEA telemetry output.
 * Format: $PYRO,seq,state,thrust,alt,vel,maxalt,press,time,flags,p1adc,p2adc,batt,temp*XX\r\n
 * SPDX-License-Identifier: MIT
 */
#include "flight_states.h"
#include "device_status.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

extern volatile device_status_t g_status;

static uint8_t telemetry_state_id(flight_state_t state) {
    switch (state) {
        case PAD_IDLE: return 0;
        case ASCENT:   return 1;
        case DESCENT:  return 2;
        case LANDED:   return 3;
        default:       return 0;
    }
}

void send_telemetry(flight_context_t *ctx, uint32_t time_ms, int32_t altitude_cm, flight_state_t state) {
    uint8_t flags = 0;
    if (ctx->pyro1_continuity_good) flags |= (1 << 0);
    if (ctx->pyro2_continuity_good) flags |= (1 << 1);
    if (ctx->pyro1_fired)           flags |= (1 << 2);
    if (ctx->pyro2_fired)           flags |= (1 << 3);
    if (ctx->pyros_armed)           flags |= (1 << 4);
    if (ctx->apogee_detected)       flags |= (1 << 5);

    uint8_t thrust = (state == ASCENT && ctx->under_thrust) ? 1 : 0;

    char payload[160];
    snprintf(payload, sizeof(payload),
             "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%u,%d",
             ctx->telemetry_seq,
             telemetry_state_id(state),
             thrust,
             (long)altitude_cm,
             (long)ctx->vertical_speed_cms,
             (long)ctx->max_altitude,
             (long)ctx->filtered_pressure,
             (unsigned long)time_ms,
             flags,
             g_status.pyro1_adc,
             g_status.pyro2_adc,
             0,
             0);

    uint8_t checksum = 0;
    for (int i = 0; payload[i] != '\0'; i++)
        checksum ^= (uint8_t)payload[i];

    char sentence[180];
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, checksum);
    uart_puts(uart0, sentence);

    ctx->telemetry_seq++;
}
