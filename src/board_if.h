/*
 * Board support contract.
 *
 * Every board support package under boards/<name>/ implements this header.
 * src/hal_common/ implements hal.h in terms of it and contains no pin
 * numbers, so a new board is a new directory rather than an edit to shared
 * code.
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

/* Called first in hal_platform_init(), before any slow initialisation
 * (USB, networking, filesystem) that could leave a pin floating.
 *
 * Put anything here that must reach a safe state immediately. On both
 * existing boards that means silencing the buzzer; on a board with pyro
 * outputs it also means driving them inactive, although the RP2040 pad
 * reset state (input, pull-down) already does that before any code runs. */
void board_early_init(void);

/* Called after the TinyUSB BSP's board_init(), which may reinitialise pins
 * the BSP believes it owns. Set up the LED, the telemetry UART pins and any
 * ADC inputs here.
 *
 * Named board_hw_init rather than board_init because bsp/board_api.h already
 * declares board_init() for the TinyUSB BSP. */
void board_hw_init(void);

/* ── Heartbeat LED ────────────────────────────────────────────────── */

/* Driven from the main loop, never from a timer or PWM peripheral: a
 * peripheral keeps blinking after the firmware has stopped, which would
 * make a dead processor look alive. */
void board_led_set(bool on);
void board_led_toggle(void);

/* ── Buzzer ───────────────────────────────────────────────────────── */

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
    uint16_t ch_a_biased;   /* T3: channel A during its own bias pulse  */
    uint16_t ch_b_biased;   /* T3: channel B during its own bias pulse  */
    uint16_t vbat;          /* pack voltage                             */
    uint16_t bus_decay_tau_us; /* bus decay time constant after bias release,
                                * 0 if the board does not measure it       */
} board_pyro_raw_t;

bool board_pyro_raw(board_pyro_raw_t *out);

/* Optional high-speed sense capture, for bench characterisation.
 *
 * board_pyro_wave_request() queues one capture (charge or discharge of
 * whatever node the board considers its pyro bus) and returns false if the
 * board does not support it. The capture and the file write happen in the
 * main loop, not in the caller's context, because the write touches flash
 * and DECISIONS.md #2 keeps flash I/O out of the USB/network service path.
 *
 * The board writes the samples to littlefs as CSV, which the existing
 * chunked file route then serves, so the size is not bounded by TCP_SND_BUF.
 * Poll board_pyro_wave_state() until it reads 2, then GET the file. */
/* mode: 0 = charge (bias step on), 1 = decay (bias step off),
 *       2 = arm (run the arm element, capture precharge and disarm).
 * Mode 2 is gated by the board's own arm interlock and may refuse. */
bool board_pyro_wave_request(int mode);
int board_pyro_wave_state(void); /* 0 idle, 1 busy, 2 ready */

/* ── Telemetry UART ───────────────────────────────────────────────── */

/* The UART instance used for telemetry TX and ground-test RX, and its
 * IRQ number. hal_common owns the ISR-driven TX ring buffer; the board
 * only says which peripheral and which pins. */
uart_inst_t *board_uart(void);
uint board_uart_irq(void);

#endif
