/*
 * MS5607 one-shot pair [DD-051, DD-066].
 *
 * The loop starts a pair and, a loop later, takes it: a pressure, then a
 * temperature. Between the two an interrupt state machine owns the sensor:
 * the handler sends each command, stamps it from the hardware timer, arms an
 * alarm for the conversion's end, and at the alarm reads the ADC -- and, after
 * the pressure, commands the temperature at once. The stamps are the
 * handler's, taken as each conversion begins, so neither a late loop nor a
 * flash erase that holds a read off moves them [SNS-PRES-08].
 *
 * The handler runs from RAM, and ms5607_bus.h forces everything it calls
 * inline. The firmware's bus is src/hal_common/ms5607_bus.h; ms5607_tests
 * builds this file against a fake one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_bus.h"
#include "ms5607_driver.h"
#include "loop_period.h"

_Static_assert(2u * MS5607_CONV_MS <= LOOP_PERIOD_MS, "a pair a loop");

#define MS5607_CMD_CONV_D1 0x48u
#define MS5607_CMD_CONV_D2 0x58u

enum { ONESHOT_IDLE, ONESHOT_COMMAND, ONESHOT_PRESSURE, ONESHOT_TEMPERATURE };

static struct {
    volatile uint8_t state;
    volatile bool ready; /* finished; the loop has not taken it */
    ms5607_pair_t pair;
    uint32_t flash_ops; /* ms5607_bus_flash_ops() as the conversion began */
    int alarm;          /* -1: not begun */
} oneshot = {.alarm = -1};

/* Commands a conversion and arms its read. False: the sensor did not answer.
 * Forced inline: the handler's whole closure must be in RAM. */
__force_inline static bool convert(uint8_t cmd, uint64_t *at_us) {
    if (!ms5607_bus_command(cmd))
        return false;
    uint64_t began = ms5607_bus_now_us();
    oneshot.flash_ops = ms5607_bus_flash_ops();
    *at_us = began + MS5607_HALF_CONV_US;
    ms5607_bus_alarm_at(oneshot.alarm, began + MS5607_CONV_DONE_US);
    return true;
}

static void __noinline __not_in_flash_func(ms5607_alarm_isr)(void) {
    ms5607_bus_alarm_ack(oneshot.alarm);
    ms5607_pair_t *p = &oneshot.pair;
    switch (oneshot.state) {
    case ONESHOT_COMMAND:
        if (convert(MS5607_CMD_CONV_D1, &p->d1_at_us)) {
            oneshot.state = ONESHOT_PRESSURE;
            return;
        }
        break;
    case ONESHOT_PRESSURE:
        p->d1_flashed = ms5607_bus_flash_ops() != oneshot.flash_ops;
        if (ms5607_bus_read_adc(&p->d1) && convert(MS5607_CMD_CONV_D2, &p->d2_at_us)) {
            oneshot.state = ONESHOT_TEMPERATURE;
            return;
        }
        break;
    case ONESHOT_TEMPERATURE:
        p->d2_flashed = ms5607_bus_flash_ops() != oneshot.flash_ops;
        p->ok = ms5607_bus_read_adc(&p->d2);
        break;
    default:
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

ms5607_start_t ms5607_async_start(void) {
    if (oneshot.alarm < 0)
        return MS5607_NOT_BEGUN;
    if (oneshot.state != ONESHOT_IDLE)
        return MS5607_BUSY;
    oneshot.pair = (ms5607_pair_t){0};
    __dmb();
    oneshot.state = ONESHOT_COMMAND;
    ms5607_bus_alarm_now(oneshot.alarm);
    return MS5607_STARTED;
}

bool ms5607_async_take(ms5607_pair_t *out) {
    if (!oneshot.ready)
        return false;
    __dmb();
    *out = oneshot.pair;
    oneshot.ready = false;
    return true;
}

bool ms5607_async_cycle(ms5607_temps_t *t, ms5607_pair_t *out, ms5607_start_t *started) {
    bool took = ms5607_async_take(out);
    if (took && !out->ok) {
        *started = MS5607_HELD;
        return true;
    }
    if (took && out->d2 != 0 && !out->d2_flashed)
        ms5607_temps_note(t, out->d2, out->d2_at_us);
    *started = ms5607_async_start();
    return took;
}
