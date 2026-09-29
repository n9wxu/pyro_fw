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
 * Never from the flight task or an interrupt: refused and counted. Before
 * the scheduler starts only core0 runs, and an operation runs directly.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_OP_H
#define FLASH_OP_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*flash_op_fn)(void *arg);

/* 0, or <0: refused (the flight task, an interrupt, a board that says no for
 * too long) or a helper that would not park. */
int flash_op(flash_op_fn fn, void *arg);

/* Offsets from the start of flash, as flash_range_*() take them. */
int flash_op_erase(uint32_t offset, uint32_t len);
int flash_op_program(uint32_t offset, const uint8_t *data, uint32_t len);

/* Creates the lockout helpers. Before the scheduler starts. */
void flash_op_init(void);

/* Weak and always true. A board whose pyro sequence is paced by the flight
 * task says no while it runs: an erase stops the flight task for tens of
 * milliseconds, longer than MK1C's arm pump coasts [DD-056]. flash_op()
 * waits for it, bounded. */
bool board_flash_ok(void);

/* [DD-068] Advanced by every erase and program, before interrupts return.
 * An operation's current disturbs a pressure conversion running beside it
 * (G4-M), so a sensor notes this as a conversion starts and compares it as
 * the conversion is read. A plain RAM word: the MS5607's handler reads it. */
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
