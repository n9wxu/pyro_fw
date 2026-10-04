/*
 * The pressure collector's bus and clock, faked for collector_tests. The
 * firmware's is src/hal_common/collector_bus.h.
 *
 * The clock moves only when the test moves it. An interrupt is the test
 * calling fake_bus.handler. A transfer takes its bit times at the wiring's
 * rate, and a part (the test's) is told of each one.
 */
#ifndef COLLECTOR_BUS_H
#define COLLECTOR_BUS_H

#include "pressure_collector.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define __not_in_flash_func(f) f
#define __noinline __attribute__((noinline))
#define __force_inline inline __attribute__((always_inline))
#define COLLECTOR_BUS_HANDLER

#define FAKE_BUS_NEVER UINT64_MAX

typedef struct {
    uint64_t now;
    void (*handler)(void);
    collector_wiring_t wiring;
    bool armed, forced;
    uint64_t alarm_at;
    int held_off; /* the task's side holds the handler off */
    uint32_t flash_ops;

    /* The transfer on the bus. */
    bool is_read;
    uint8_t read_len;
    uint64_t done_at;
    collector_cause_t ended; /* how it will have ended */
    uint8_t answer[COLLECTOR_READ_MAX];
    uint32_t aborts;

    /* Faults. */
    collector_cause_t fail; /* COLLECTOR_OK: none */
    int fail_transfers;     /* how many more fail; negative: all */
    int sda_stuck_clocks;   /* SDA is held low until this many clocks */

    /* The lines, while the machine has them. */
    bool lines_taken, scl_high, sda_high;
    int clocks, stops, takes, gives;
    uint64_t last_edge_at, shortest_half_bit;
} fake_bus_t;

extern fake_bus_t fake_bus;

/* The part on the bus. */
void fake_part_written(const uint8_t *bytes, uint8_t len, uint64_t ended_at);
void fake_part_read(uint8_t from_register, uint8_t *data, uint8_t len, uint64_t at);

static inline uint64_t fake_bus_bit_us(void) {
    return (1000000u + fake_bus.wiring.hz - 1u) / fake_bus.wiring.hz;
}

static inline uint64_t collector_bus_now_us(void) {
    return fake_bus.now;
}

static inline uint32_t collector_bus_flash_ops(void) {
    return fake_bus.flash_ops;
}

static inline void collector_bus_alarm_at(uint64_t at_us) {
    fake_bus.alarm_at = at_us;
    fake_bus.armed = true;
}

static inline void collector_bus_alarm_now(void) {
    fake_bus.forced = true;
}

static inline void collector_bus_alarm_ack(void) {
    fake_bus.forced = false;
}

static inline void collector_bus_hold_off(void) {
    fake_bus.held_off++;
}

static inline void collector_bus_let_in(void) {
    fake_bus.held_off--;
}

static inline collector_cause_t fake_bus_fault(void) {
    if (fake_bus.sda_stuck_clocks > 0)
        return COLLECTOR_LINE_HELD;
    if (fake_bus.fail == COLLECTOR_OK || fake_bus.fail_transfers == 0)
        return COLLECTOR_OK;
    if (fake_bus.fail_transfers > 0)
        fake_bus.fail_transfers--;
    return fake_bus.fail;
}

static inline void collector_bus_write(const uint8_t *bytes, uint8_t len) {
    fake_bus.is_read = false;
    fake_bus.ended = fake_bus_fault();
    fake_bus.done_at = fake_bus.now + (9u * (len + 1u) + 2u) * fake_bus_bit_us();
    if (fake_bus.ended == COLLECTOR_TIMEOUT)
        fake_bus.done_at = FAKE_BUS_NEVER;
    if (fake_bus.ended == COLLECTOR_OK)
        fake_part_written(bytes, len, fake_bus.done_at);
}

static inline void collector_bus_read(uint8_t from_register, uint8_t len) {
    fake_bus.is_read = true;
    fake_bus.read_len = len;
    fake_bus.ended = fake_bus_fault();
    fake_bus.done_at = fake_bus.now + (9u * (len + 3u) + 3u) * fake_bus_bit_us();
    if (fake_bus.ended == COLLECTOR_TIMEOUT)
        fake_bus.done_at = FAKE_BUS_NEVER;
    if (fake_bus.ended == COLLECTOR_OK)
        fake_part_read(from_register, fake_bus.answer, len, fake_bus.now);
}

static inline collector_cause_t collector_bus_result(uint8_t *data, uint8_t len) {
    if (fake_bus.now < fake_bus.done_at) {
        fake_bus.aborts++;
        return COLLECTOR_TIMEOUT;
    }
    if (fake_bus.ended == COLLECTOR_OK && len)
        memcpy(data, fake_bus.answer, len);
    return fake_bus.ended;
}

static inline void fake_bus_edge(void) {
    uint64_t since = fake_bus.now - fake_bus.last_edge_at;
    if (since < fake_bus.shortest_half_bit)
        fake_bus.shortest_half_bit = since;
    fake_bus.last_edge_at = fake_bus.now;
}

static inline void collector_bus_lines_take(void) {
    fake_bus.lines_taken = true;
    fake_bus.scl_high = fake_bus.sda_high = true;
    fake_bus.takes++;
    fake_bus.last_edge_at = fake_bus.now;
}

static inline void collector_bus_scl(bool high) {
    fake_bus_edge();
    if (high && !fake_bus.scl_high) {
        fake_bus.clocks++;
        if (fake_bus.sda_stuck_clocks > 0)
            fake_bus.sda_stuck_clocks--;
    }
    fake_bus.scl_high = high;
}

/* SDA rising while SCL is high is a STOP. */
static inline void collector_bus_sda(bool high) {
    fake_bus_edge();
    if (high && !fake_bus.sda_high && fake_bus.scl_high)
        fake_bus.stops++;
    fake_bus.sda_high = high;
}

static inline void collector_bus_lines_give(void) {
    fake_bus.lines_taken = false;
    fake_bus.gives++;
}

static inline bool collector_bus_begin(const collector_wiring_t *w, void (*handler)(void)) {
    fake_bus.wiring = *w;
    fake_bus.handler = handler;
    return true;
}

#endif /* COLLECTOR_BUS_H */
