/*
 * The pressure collector [SNS-COL-01..06, DD-093].
 *
 * One interrupt state machine owns the sensor and its bus. It runs free:
 * command a conversion, wait out the part's own conversion time on an alarm,
 * read, and command the next, with no start from any task. Each finished
 * cycle is the part's raw bytes and the time of each conversion, put on a
 * queue of four. A task takes them and does the arithmetic.
 *
 * A read can only follow its own command by the conversion time, so no read
 * is early and none is repeated. A transfer that fails is counted by cause
 * and the cycle starts again; three in a row and the machine clears the bus
 * and resets the part itself.
 *
 * The handler and everything it calls run from RAM (collector_bus.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRESSURE_COLLECTOR_H
#define PRESSURE_COLLECTOR_H

#include <stdbool.h>
#include <stdint.h>

#define COLLECTOR_STEPS_MAX 2
#define COLLECTOR_READ_MAX 6
#define COLLECTOR_COMMAND_MAX 2
#define COLLECTOR_QUEUE 4

/* One conversion: what starts it, how long it takes, and where its result
 * is read from. */
typedef struct {
    uint8_t command[COLLECTOR_COMMAND_MAX];
    uint8_t command_len;
    uint8_t read_register;
    uint8_t read_len;
    uint32_t convert_us; /* the part's worst case, from the end of the command */
    uint32_t middle_us;  /* when the result describes, from the end of the command */
} collector_step_t;

typedef struct {
    uint8_t steps;
    collector_step_t step[COLLECTOR_STEPS_MAX];
    uint8_t reset[COLLECTOR_COMMAND_MAX];
    uint8_t reset_len;
    uint32_t reset_us; /* until the part answers again */
} collector_part_t;

/* Where the part is. */
typedef struct {
    uint8_t i2c; /* the controller's number */
    uint8_t address;
    uint8_t scl_pin, sda_pin;
    uint32_t hz;
} collector_wiring_t;

/* One cycle, as the part gave it. */
typedef struct {
    uint8_t data[COLLECTOR_STEPS_MAX][COLLECTOR_READ_MAX];
    uint64_t at_us[COLLECTOR_STEPS_MAX]; /* [SNS-PRES-08] the middle of each conversion */
    bool flashed[COLLECTOR_STEPS_MAX];   /* [SNS-PRES-14] a flash operation ran beside it */
} collector_raw_t;

/* Why a transfer failed, as the bus controller reports it. */
typedef enum {
    COLLECTOR_OK,
    COLLECTOR_ADDRESS_NACK, /* the part did not answer its address */
    COLLECTOR_DATA_NACK,    /* it refused a byte */
    COLLECTOR_LINE_HELD,    /* SDA was low when the controller let it go */
    COLLECTOR_TIMEOUT,      /* the transfer did not finish in its time */
    COLLECTOR_CAUSES
} collector_cause_t;

typedef struct {
    uint32_t cycles;                   /* put on the queue */
    uint32_t dropped;                  /* pushed out of it, oldest first, untaken */
    uint32_t failed[COLLECTOR_CAUSES]; /* transfers, by cause */
    uint32_t recoveries;               /* bus clears and part resets */
} collector_stats_t;

/* Starts the machine. False: no hardware alarm was free. */
bool collector_begin(const collector_part_t *part, const collector_wiring_t *wiring);

/* The oldest cycle not yet taken. */
bool collector_take(collector_raw_t *out);

void collector_stats(collector_stats_t *out);

/* A cycle's time when nothing fails, on this wiring. */
uint32_t collector_cycle_us(const collector_part_t *part, const collector_wiring_t *wiring);

#endif
