/*
 * Board support contract.
 *
 * Every board support package under boards/<name>/ implements this header.
 * src/hal_common/ implements hal.h in terms of it and contains no pin
 * numbers, so a new board is a new directory rather than an edit to shared
 * code.
 *
 * Only board_early_init() has no default. The rest have plain-GPIO defaults
 * in src/hal_common/board_defaults.c, on the pins board_pins.h names; a
 * board whose hardware differs defines its own and that one links.
 *
 * Scope: this contract assumes an RP2040-family target, because
 * src/hal_common/ is written against the Pico SDK. Porting to a different
 * MCU family means supplying a new common HAL alongside a new board
 * directory; the board's CMakeLists.txt chooses which one it links.
 *
 * To add a board:
 *   1. cp -r boards/reference boards/<name>
 *   2. Implement the functions below, plus pyro.h and pressure_sensor.h
 *   3. Edit boards/<name>/board.cmake and CMakeLists.txt
 *   4. cmake -B build-<name> -DPYRO_BOARD=<name> && cmake --build build-<name>
 *
 * The top-level CMakeLists.txt never changes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_IF_H
#define BOARD_IF_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/uart.h"

/* ── Lifecycle ────────────────────────────────────────────────────── */

/* Called in hal_platform_init() once the buzzer is silenced, before any
 * slow initialisation (USB, networking, filesystem). Drive every pyro output
 * inactive here. The RP2040's pad reset state (input, pull-down) already
 * holds them there, so this asserts the safe state rather than making it. */
void board_early_init(void);

/* Called after the TinyUSB BSP's board_init(), which reinitialises the pin
 * PICO_DEFAULT_LED_PIN names. The default sets up the LED and the telemetry
 * UART pins. Not board_init(): bsp/board_api.h declares that. */
void board_hw_init(void);

/* ── Heartbeat LED ────────────────────────────────────────────────── */

/* Driven from the main loop, never from a timer or PWM peripheral: a
 * peripheral keeps blinking after the firmware has stopped, which would
 * make a dead processor look alive. */
void board_led_set(bool on);
void board_led_toggle(void);

/* ── Buzzer ───────────────────────────────────────────────────────── */

/* No pad is driving the buzzer. MK1A boots like this: it fits no buzzer, and
 * declares no pin rather than invite someone to drive a pad that goes
 * nowhere. Assigning one in pins.ini is how that board gets a buzzer. */
#define BOARD_BUZZER_NO_PIN 255u

/* Move the buzzer to another pad, or BOARD_BUZZER_NO_PIN to silence it.
 *
 * Runtime rather than compile-time: an operator may wire a buzzer to a user
 * pad, the only way MK1A has one at all. The old pad returns to input, so a
 * reassignment never leaves two pads driven. pin_assign_validate() has
 * already checked the pad can take it. The default starts on
 * BOARD_PIN_BUZZER, or on no pad when board_pins.h declares none. */
void board_buzzer_set_pin(uint8_t pin);

void board_buzzer_init(void);
void board_buzzer_on(void);
void board_buzzer_off(void);

/* ── Bench diagnostics ────────────────────────────────────────────── */

/* Raw pyro sense values, reported verbatim by /api/status.
 *
 * Exists because a boolean hides what matters: a degraded match or a
 * partially conducting FET sits between the thresholds, and only the raw
 * count shows it. Report counts, not volts, so nothing is lost to rounding.
 *
 * bus_quiescent is the firing bus with NO stimulus applied. It is the
 * safety-relevant one: a bus sitting near the pack voltage with nothing
 * driving it means the high side has failed short (DESIGN.md 8.1).
 * bus_biased is the same node during the bias pulse, which measures the
 * bus pull-down network instead.
 *
 * Fill in whatever the board actually has and return true. A board with no
 * analog pyro sensing returns false and the fields are omitted. */
typedef struct {
    uint16_t bus_quiescent; /* T1: firing bus with NO stimulus applied */
    uint16_t bus_biased;    /* T2: firing bus during the bus-bias pulse */
    uint16_t vbat;          /* pack voltage                             */
} board_pyro_raw_t;

bool board_pyro_raw(board_pyro_raw_t *out);

/* ── Telemetry UART ───────────────────────────────────────────────── */

/* The UART instance used for telemetry TX and ground-test RX, and its
 * IRQ number. hal_common owns the ISR-driven TX ring buffer; the board
 * only says which peripheral and which pins. */
uart_inst_t *board_uart(void);
uint board_uart_irq(void);

#endif
