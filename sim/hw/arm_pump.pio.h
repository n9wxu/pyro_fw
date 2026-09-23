/*
 * Host stand-in for the header pioasm generates from
 * boards/mk1c/arm_pump.pio.
 *
 * The real generated header carries the assembled instruction words. The
 * shim's PIO model is behavioural rather than instruction-level (see
 * rp2040_shim.h), so what it needs from this file is the program's shape
 * and its init function, with the same signature the .pio file's c-sdk
 * block declares -- boards/mk1c/pyro_board.c calls it directly.
 *
 * Kept in step with the .pio by hand. If the pump's timing changes there,
 * the 50-clocks-per-cycle constant in rp2040_shim.c changes with it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ARM_PUMP_PIO_H
#define ARM_PUMP_PIO_H

#include "hardware/pio.h"

/* 4 instructions: pull, mov, set, set, jmp -- the length is all the shim
 * uses it for. */
static const pio_program_t arm_pump_program = {.length = 5};

static inline pio_sm_config arm_pump_program_get_default_config(uint offset) {
    (void)offset;
    pio_sm_config c = {0, 0, 1.0f};
    return c;
}

static inline void arm_pump_program_init(PIO pio, uint sm, uint offset, uint pin, float clkdiv) {
    pio_sm_config c = arm_pump_program_get_default_config(offset);
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    sm_config_set_set_pins(&c, pin, 1);
    sm_config_set_clkdiv(&c, clkdiv);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_enabled(pio, sm, true);
}

#endif
