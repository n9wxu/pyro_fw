/*
 * MS5607 one-shot conversion [DD-051].
 *
 * The loop starts a conversion and, a loop later, takes it. Between the two an
 * interrupt state machine owns the sensor: the handler sends the command,
 * stamps it from the hardware timer, arms an alarm for the conversion's end,
 * and at the alarm reads the ADC. The stamp is the handler's, taken as the
 * conversion begins, so neither a late loop nor a flash erase that holds the
 * read off moves it [SNS-PRES-08].
 *
 * The handler runs from RAM, and ms5607_bus.h forces everything it calls
 * inline. The firmware's bus is src/hal_common/ms5607_bus.h; ms5607_tests
 * builds this file against a fake one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_bus.h"
#include "ms5607_driver.h"

#define MS5607_CMD_CONV_D1 0x48u
#define MS5607_CMD_CONV_D2 0x58u

enum { ONESHOT_IDLE, ONESHOT_COMMAND, ONESHOT_CONVERTING };

static struct {
    volatile uint8_t state;
    volatile bool ready; /* finished; the loop has not taken it */
    bool temperature;
    bool ok;
    uint32_t raw;
    uint64_t at_us;
    int alarm; /* -1: not begun */
} oneshot = {.alarm = -1};

static void __noinline __not_in_flash_func(ms5607_alarm_isr)(void) {
    ms5607_bus_alarm_ack(oneshot.alarm);
    if (oneshot.state == ONESHOT_COMMAND) {
        if (ms5607_bus_command(oneshot.temperature ? MS5607_CMD_CONV_D2 : MS5607_CMD_CONV_D1)) {
            uint64_t began = ms5607_bus_now_us();
            oneshot.at_us = began + MS5607_HALF_CONV_US;
            oneshot.state = ONESHOT_CONVERTING;
            ms5607_bus_alarm_at(oneshot.alarm, began + MS5607_CONV_DONE_US);
            return;
        }
        oneshot.ok = false;
        oneshot.raw = 0;
    } else if (oneshot.state == ONESHOT_CONVERTING) {
        uint32_t raw = 0;
        oneshot.ok = ms5607_bus_read_adc(&raw);
        oneshot.raw = raw;
    } else {
        return;
    }
    __dmb();
    oneshot.ready = true;
    oneshot.state = ONESHOT_IDLE;
}

bool ms5607_async_begin(void) {
    if (oneshot.alarm >= 0)
        return true;
    return ms5607_bus_begin(ms5607_address(), ms5607_alarm_isr, &oneshot.alarm);
}

ms5607_start_t ms5607_async_start(bool temperature) {
    if (oneshot.alarm < 0)
        return MS5607_NOT_BEGUN;
    if (oneshot.state != ONESHOT_IDLE)
        return MS5607_BUSY;
    oneshot.temperature = temperature;
    __dmb();
    oneshot.state = ONESHOT_COMMAND;
    ms5607_bus_alarm_now(oneshot.alarm);
    return MS5607_STARTED;
}

bool ms5607_async_take(ms5607_conversion_t *out) {
    if (!oneshot.ready)
        return false;
    __dmb();
    out->raw = oneshot.raw;
    out->at_us = oneshot.at_us;
    out->temperature = oneshot.temperature;
    out->ok = oneshot.ok;
    oneshot.ready = false;
    return true;
}

bool ms5607_async_cycle(ms5607_temps_t *t, ms5607_conversion_t *out, ms5607_start_t *started) {
    bool took = ms5607_async_take(out);
    if (took && !out->ok) {
        *started = MS5607_HELD;
        return true;
    }
    if (took && out->temperature && out->raw != 0)
        ms5607_temps_note(t, out->raw, out->at_us);
    bool temperature = ms5607_temperature_due(t);
    *started = ms5607_async_start(temperature);
    if (*started == MS5607_STARTED)
        ms5607_conversion_started(t, temperature);
    return took;
}
