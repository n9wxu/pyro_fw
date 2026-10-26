/*
 * Flash operations under FreeRTOS [DD-074].
 *
 * Programming or erasing the QSPI flash takes it out of execute-in-place, so
 * nothing on either core may fetch from flash meanwhile: not a task, not an
 * interrupt handler. flash_op() arranges that for one operation:
 *
 *   1. the caller's priority rises to T. Under mode 0 every task at P, on
 *      both cores, yields -- the flight task included;
 *   2. the other core's lockout helper, a task at T that exists from boot,
 *      is woken; it disables that core's interrupts and spins in RAM;
 *   3. the caller disables its own interrupts and runs the operation;
 *   4. the helper is released, and the caller's priority restored.
 *
 * Every wait in it is bounded. A helper that does not park in time costs a
 * refused operation, never a hang, and a parked helper releases itself after
 * PARK_MAX_US whatever happens.
 *
 * Why the caller must be at T (plan 2, 4.2 rule 1): under mode 0, a core
 * runs only tasks of the highest ready priority. A caller at P with a helper
 * ready at T would be preempted and never run again until the helper gave
 * up. The SDK's flash_safe_execute() gets this right only when its caller is
 * already at T, and creates a task per call; the helpers here exist once.
 *
 * Never from the flight task or an interrupt, and never from launch to
 * landing (below): refused and counted. Before the scheduler starts only
 * core0 runs, and an operation runs directly.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_OP_H
#define FLASH_OP_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*flash_op_fn)(void *arg);

/* 0, or <0: refused (the flight task, an interrupt, a board that says no for
 * too long, a flight in progress) or a helper that would not park. */
int flash_op(flash_op_fn fn, void *arg);

/* Offsets from the start of flash, as flash_range_*() take them. */
int flash_op_erase(uint32_t offset, uint32_t len);
int flash_op_program(uint32_t offset, const uint8_t *data, uint32_t len);

/* Creates the lockout helpers. Before the scheduler starts. */
void flash_op_init(void);

/* ── From launch to landing [FLT-RT-03, FLT-LOG-09, DD-104] ──────────
 *
 * The flight task's core runs only from RAM (FLT-RT-02), so a flash
 * operation needs no lockout: the log writer's run on core1 with core1's
 * interrupts off, in pieces inside its slots (flash_slot.h), and stop
 * nothing on core0. Every other operation is refused until landing.
 *
 * Begin and end are the flight task's, at launch and at landing; end waits
 * out a piece in progress, a slot's length at most, before the flight task
 * runs from flash again. */
void flash_op_flight_begin(void);
void flash_op_flight_end(void);
bool flash_op_in_flight(void);

/* Whether an erase asked for now can run: on the ground always; from launch
 * to landing only on a part whose erase ends in suspended pieces
 * [FLT-LOG-10]. An erase refused is a sector the filesystem takes for bad. */
bool flash_op_can_erase(void);

/* The task whose operations run in slots: the storage task. */
void flash_op_set_writer(void *task);

/* One sector erase, or one program of exactly len bytes inside one page,
 * whose other bytes it leaves alone, by the writer: in pieces inside its
 * slots from launch to landing; on the ground the writer's erase, where the
 * part's erase suspends, in the same pieces with core0 parked for each
 * [FLT-RT-06]; whole under the lockout otherwise. 0, or <0 as flash_op() and
 * flash_slot_run(). */
int flash_op_slot_erase(uint32_t offset);
int flash_op_slot_program(uint32_t offset, const uint8_t *data, uint32_t len);

/* /api/status: pieces run in slots, of which suspended; slots given up
 * waiting; operations refused in flight; the longest piece. */
typedef struct {
    uint32_t pieces, suspends, slot_waits, flight_refusals, piece_max_us;
} flash_op_slot_stats_t;
void flash_op_slot_stats(flash_op_slot_stats_t *out);

/* Weak and always true. A board whose pyro sequence is paced by the flight
 * task says no while it runs: an erase stops the flight task for tens of
 * milliseconds, longer than MK1C's arm pump coasts [DD-056]. flash_op()
 * waits for it, bounded. */
bool board_flash_ok(void);

/* [DD-106] Advanced by every erase and program, before interrupts return.
 * An operation's current disturbs a pressure conversion running beside it
 * (G4-M), so the pressure collector notes this as a conversion starts and
 * compares it as the conversion is read. A plain RAM word: the collector's
 * alarm handler reads it (collector_bus.h). */
extern volatile uint32_t flash_op_seq;

/* /api/status. */
uint32_t flash_op_lockouts(void); /* operations done under a lockout      */
uint32_t flash_op_timeouts(void); /* helpers that did not park in time    */
uint32_t flash_op_refusals(void); /* from the flight task or an interrupt */
uint32_t flash_op_waits(void);    /* operations that waited on the board  */
uint32_t flash_op_erases(void);
uint32_t flash_op_programs(void);
uint32_t flash_op_max_us(void); /* the longest the system stopped for one */

/* Decoded by the safe-boot latch like a flight task stage number. */
void flash_op_crumb(unsigned n);

#endif
