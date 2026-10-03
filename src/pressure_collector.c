/*
 * See pressure_collector.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pressure_collector.h"
#include "collector_bus.h"

#define FAILURES_BEFORE_RECOVERY 3u
#define RETRY_US 1000u
/* The bus clear of UM10204 3.1.16: nine clocks, then a STOP. */
#define CLEAR_CLOCK_EDGES 18u
#define CLEAR_HALF_BIT_US 10u
/* A transfer's allowance past its own bit times. */
#define TRANSFER_MARGIN_US 100u

typedef enum {
    IDLE,
    COMMANDED, /* a conversion's command is on the bus, then the part converts */
    READING,
    RETRYING,
    CLEARING, /* the lines are the machine's: clocks, then a STOP */
    RESETTING,
    RELOADING, /* the part is starting again */
} state_t;

static struct {
    volatile uint8_t state;
    collector_part_t part; /* a copy: the handler reads nothing in flash */
    uint32_t bit_us;
    uint8_t step;
    uint8_t clear_edge;
    uint8_t failures_in_a_row;
    uint32_t flash_ops; /* as the present conversion began */
    /* The cycles to take, oldest at head, and after them the one being
     * built: the handler copies nothing. */
    collector_raw_t queue[COLLECTOR_QUEUE + 1];
    volatile uint8_t head, count;
    collector_stats_t stats;
} machine;

/* START, the address, each byte with its acknowledge, and STOP. */
__force_inline static uint32_t write_bits(uint8_t len) {
    return 9u * (len + 1u) + 2u;
}

/* The register's address written, a repeated START, then the bytes. */
__force_inline static uint32_t read_bits(uint8_t len) {
    return 9u * (len + 3u) + 3u;
}

__force_inline static uint32_t bit_us_at(uint32_t hz) {
    return (1000000u + hz - 1u) / hz;
}

__force_inline static uint32_t write_us(uint8_t len) {
    return write_bits(len) * machine.bit_us;
}

__force_inline static uint32_t read_us(uint8_t len) {
    return read_bits(len) * machine.bit_us;
}

__force_inline static void wait_until(uint64_t at_us, state_t then) {
    machine.state = (uint8_t)then;
    collector_bus_alarm_at(at_us);
}

#define SLOTS (COLLECTOR_QUEUE + 1u)

/* No division: the handler can call nothing, and a remainder is a call. */
__force_inline static uint8_t wrapped(uint8_t slot) {
    return slot >= SLOTS ? (uint8_t)(slot - SLOTS) : slot;
}

__force_inline static collector_raw_t *building(void) {
    return &machine.queue[wrapped((uint8_t)(machine.head + machine.count))];
}

/* The cycle being built joins the queue. Full, the oldest gives its slot up
 * to be the next one built. */
__force_inline static void push(void) {
    if (machine.count == COLLECTOR_QUEUE) {
        machine.head = wrapped((uint8_t)(machine.head + 1u));
        machine.stats.dropped++;
    } else {
        machine.count++;
    }
    machine.stats.cycles++;
}

__force_inline static void command(void) {
    const collector_step_t *s = &machine.part.step[machine.step];
    collector_bus_write(s->command, s->command_len);
    uint64_t converting_from = collector_bus_now_us() + write_us(s->command_len);
    machine.flash_ops = collector_bus_flash_ops();
    building()->at_us[machine.step] = converting_from + s->middle_us;
    wait_until(converting_from + s->convert_us, COMMANDED);
}

__force_inline static void begin_clear(void) {
    collector_bus_lines_take();
    machine.clear_edge = 0;
    machine.stats.recoveries++;
    wait_until(collector_bus_now_us() + CLEAR_HALF_BIT_US, CLEARING);
}

__force_inline static void failed(collector_cause_t cause) {
    machine.stats.failed[cause]++;
    machine.step = 0;
    if (++machine.failures_in_a_row < FAILURES_BEFORE_RECOVERY) {
        wait_until(collector_bus_now_us() + RETRY_US, RETRYING);
        return;
    }
    machine.failures_in_a_row = 0;
    begin_clear();
}

/* One edge an alarm. After the clocks: SDA low with SCL high, then let go,
 * which is a STOP. */
__force_inline static void clear_step(void) {
    uint8_t edge = machine.clear_edge++;
    if (edge < CLEAR_CLOCK_EDGES) {
        collector_bus_scl((edge & 1u) != 0u);
    } else if (edge == CLEAR_CLOCK_EDGES) {
        collector_bus_sda(false);
    } else if (edge == CLEAR_CLOCK_EDGES + 1u) {
        collector_bus_sda(true);
    } else {
        collector_bus_lines_give();
        collector_bus_write(machine.part.reset, machine.part.reset_len);
        wait_until(collector_bus_now_us() + write_us(machine.part.reset_len) + TRANSFER_MARGIN_US, RESETTING);
        return;
    }
    wait_until(collector_bus_now_us() + CLEAR_HALF_BIT_US, CLEARING);
}

__force_inline static void after_command(void) {
    const collector_step_t *s = &machine.part.step[machine.step];
    collector_cause_t cause = collector_bus_result(NULL, 0);
    if (cause != COLLECTOR_OK) {
        failed(cause);
        return;
    }
    building()->flashed[machine.step] = collector_bus_flash_ops() != machine.flash_ops;
    collector_bus_read(s->read_register, s->read_len);
    wait_until(collector_bus_now_us() + read_us(s->read_len) + TRANSFER_MARGIN_US, READING);
}

__force_inline static void after_read(void) {
    const collector_step_t *s = &machine.part.step[machine.step];
    collector_cause_t cause = collector_bus_result(building()->data[machine.step], s->read_len);
    if (cause != COLLECTOR_OK) {
        failed(cause);
        return;
    }
    machine.failures_in_a_row = 0;
    if (++machine.step == machine.part.steps) {
        machine.step = 0;
        push();
    }
    command();
}

__force_inline static void after_reset(void) {
    collector_cause_t cause = collector_bus_result(NULL, 0);
    if (cause != COLLECTOR_OK) {
        failed(cause);
        return;
    }
    wait_until(collector_bus_now_us() + machine.part.reset_us, RELOADING);
}

/* COLLECTOR_BUS_HANDLER keeps the compiler from making this a jump table,
 * which the linker would put in flash. */
COLLECTOR_BUS_HANDLER static void __noinline __not_in_flash_func(collector_alarm_isr)(void) {
    collector_bus_alarm_ack();
    uint8_t state = machine.state;
    if (state == COMMANDED)
        after_command();
    else if (state == READING)
        after_read();
    else if (state == RETRYING || state == RELOADING)
        command();
    else if (state == CLEARING)
        clear_step();
    else if (state == RESETTING)
        after_reset();
}

bool collector_begin(const collector_part_t *part, const collector_wiring_t *wiring) {
    if (machine.state != IDLE)
        return true;
    if (!collector_bus_begin(wiring, collector_alarm_isr))
        return false;
    machine.part = *part;
    machine.bit_us = bit_us_at(wiring->hz);
    machine.step = 0;
    machine.state = (uint8_t)RETRYING;
    collector_bus_alarm_now();
    return true;
}

bool collector_take(collector_raw_t *out) {
    collector_bus_hold_off();
    bool any = machine.count > 0;
    if (any) {
        *out = machine.queue[machine.head];
        machine.head = wrapped((uint8_t)(machine.head + 1u));
        machine.count--;
    }
    collector_bus_let_in();
    return any;
}

void collector_stats(collector_stats_t *out) {
    collector_bus_hold_off();
    *out = machine.stats;
    collector_bus_let_in();
}

uint32_t collector_cycle_us(const collector_part_t *part, const collector_wiring_t *wiring) {
    uint32_t bits = 0, waits = 0;
    for (uint8_t i = 0; i < part->steps; i++) {
        const collector_step_t *s = &part->step[i];
        bits += write_bits(s->command_len) + read_bits(s->read_len);
        waits += s->convert_us + TRANSFER_MARGIN_US;
    }
    return bits * bit_us_at(wiring->hz) + waits;
}
