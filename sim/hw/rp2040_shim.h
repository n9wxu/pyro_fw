/*
 * A host stand-in for the Pico SDK functions the boards' pyro files call,
 * backed by sim/plant/, so the real files run unmodified. Not an RP2040
 * emulator.
 *
 * Virtual time advances only when board code does something that takes
 * time (sleep_ms(), busy_wait_us(), adc_read()'s 2 us conversion: RP2040
 * datasheet §4.9, 96 cycles of the 48 MHz ADC clock), and every advance
 * steps the plant. The PIO pump and an ADC-to-DMA capture run inside the
 * advance, as they run beside the CPU: the pump coasts until its FIFO
 * drains, and a capture holds its pre-trigger baseline.
 *
 * The pads behave as the SDK's: gpio_init() makes a pin an input with its
 * latch low, and a pin drives the plant only as an output.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RP2040_SHIM_H
#define RP2040_SHIM_H

#include <stdint.h>
#include <stdbool.h>

/* Advance virtual time, stepping the plant and any background peripheral.
 * Everything in the shim that costs time funnels through here. */
void shim_advance_us(uint64_t us);

uint64_t shim_now_us(void);

/* Reset the clock and every peripheral. Called from the sim board's
 * platform init, before the firmware's own init runs. */
void shim_reset(void);

/* The firmware's main loop calls this once per tick so that time passes
 * even when the board code does nothing blocking. */
void shim_tick(uint32_t ms);

/* Bring the shim clock up to the flight software's millisecond clock.
 * Advances only when behind: board code that takes time -- an ADC
 * conversion is 2 us -- has already moved the shim clock past the tick,
 * and the model must not be rewound to meet it. */
void shim_advance_to_ms(uint32_t ms);

/* Did anything arm the watchdog and then miss its deadline? The shim does
 * not reboot on it -- there is nothing to reboot -- but a test that cares
 * whether the arm window would have survived can ask. */
bool shim_watchdog_expired(void);

#endif
