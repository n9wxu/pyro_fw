/*
 * The MS5607 one-shot's bus and clock, faked for ms5607_tests. The firmware's
 * is src/hal_common/ms5607_bus.h: i2c1 and a timer alarm, register by register.
 *
 * The clock moves only when the test moves it, or by what a bus transfer takes
 * at 100 kHz. An interrupt is the test calling fake_bus_handler.
 */
#ifndef MS5607_BUS_H
#define MS5607_BUS_H

#include <stdbool.h>
#include <stdint.h>

#define __not_in_flash_func(f) f
#define __noinline __attribute__((noinline))
#define __dmb() __sync_synchronize()

#define FAKE_BUS_COMMAND_US 200u /* a byte and a STOP */
#define FAKE_BUS_READ_US 600u    /* 0x00, a restart and three bytes back */

extern uint64_t fake_bus_now;
extern uint32_t fake_bus_adc; /* what the next ADC read returns */
extern bool fake_bus_nack;    /* the sensor does not answer */
extern uint8_t fake_bus_cmds[64];
extern int fake_bus_ncmds;
extern int fake_bus_reads;
extern uint64_t fake_bus_alarm_at;
extern bool fake_bus_armed;  /* an alarm is set and has not fired */
extern bool fake_bus_forced; /* the interrupt is pending now */
extern uint8_t fake_bus_address;
extern void (*fake_bus_handler)(void);

static inline uint64_t ms5607_bus_now_us(void) {
    return fake_bus_now;
}

static inline bool ms5607_bus_command(uint8_t cmd) {
    fake_bus_now += FAKE_BUS_COMMAND_US;
    if (fake_bus_nack)
        return false;
    if (fake_bus_ncmds < (int)sizeof(fake_bus_cmds))
        fake_bus_cmds[fake_bus_ncmds++] = cmd;
    return true;
}

static inline bool ms5607_bus_read_adc(uint32_t *value) {
    fake_bus_now += FAKE_BUS_READ_US;
    fake_bus_reads++;
    if (fake_bus_nack)
        return false;
    *value = fake_bus_adc;
    return true;
}

static inline void ms5607_bus_alarm_at(int alarm, uint64_t at_us) {
    (void)alarm;
    fake_bus_alarm_at = at_us;
    fake_bus_armed = true;
}

static inline void ms5607_bus_alarm_now(int alarm) {
    (void)alarm;
    fake_bus_forced = true;
}

static inline void ms5607_bus_alarm_ack(int alarm) {
    (void)alarm;
    fake_bus_forced = false;
}

static inline bool ms5607_bus_begin(uint8_t address, void (*handler)(void), int *alarm) {
    fake_bus_address = address;
    fake_bus_handler = handler;
    *alarm = 0;
    return true;
}

#endif /* MS5607_BUS_H */
