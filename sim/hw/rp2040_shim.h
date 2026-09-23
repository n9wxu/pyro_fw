/*
 * A host stand-in for the part of the Pico SDK that the pyro board files
 * touch, backed by sim/plant/.
 *
 * This is not an RP2040 emulator. It is the smallest surface that lets
 * boards/mk1a|mk1b|mk1c/pyro_board.c compile and run unmodified on the
 * host: about thirty functions across GPIO, the ADC, DMA, PIO and the
 * timers. Running the real board file is the whole point -- the thresholds,
 * the settle times, the median-of-3, the state machine that replaced two
 * sleeps -- none of that is exercised by a reimplementation.
 *
 * ── The clock is the interesting part ────────────────────────────
 *
 * Virtual time advances only when the board code does something that takes
 * time, and every advance steps the plant. That is what makes the model
 * answer like hardware instead of like a table:
 *
 *   sleep_ms(8)      advances 8 ms, so a bias node settles 8 ms worth
 *   adc_read()       advances 2 us, one 96-cycle conversion at 48 MHz
 *   busy_wait_us(n)  advances n us
 *
 * adc_read() costing time is load-bearing rather than pedantic. MK1C's
 * bus_decay_probe() times a decay by counting adc_read() calls until the
 * level crosses 1/e; with a free conversion that loop measures zero and
 * the probe silently reports nonsense. With the conversion priced at 2 us
 * it measures the same thing it measures on the bench.
 *
 * ── Two peripherals run in the background ────────────────────────
 *
 * The PIO state machine and an armed ADC-to-DMA capture both keep running
 * while the CPU is doing something else, so both are driven from inside
 * the time advance rather than from their own API calls. For the pump that
 * is what reproduces the coast DESIGN.md's passive-disarm argument depends
 * on: when the CPU stops pushing, the SM keeps toggling until the FIFO
 * drains and only then stalls. For the capture it is what puts the
 * pre-trigger baseline in the buffer, since the firmware arms the DMA,
 * busy-waits 200 us, and only then moves the pin.
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
 * Advances only when behind: board code that blocks -- MK1C's 8 ms bias
 * pulses, MK1A's medians -- has already moved the shim clock past the
 * tick, and the model must not be rewound to meet it. */
void shim_advance_to_ms(uint32_t ms);

/* Did anything arm the watchdog and then miss its deadline? The shim does
 * not reboot on it -- there is nothing to reboot -- but a test that cares
 * whether the arm window would have survived can ask. */
bool shim_watchdog_expired(void);

#endif
