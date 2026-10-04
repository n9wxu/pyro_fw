/*
 * Host stand-in for the header pioasm generates from
 * boards/mk1c/arm_pump.pio: the program's length and its init function, with
 * the .pio's c-sdk signature. Kept in step with the .pio by hand, with
 * ARM_PUMP_CLOCKS_PER_CYCLE in rp2040_shim.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ARM_PUMP_PIO_H
#define ARM_PUMP_PIO_H

#include "hardware/pio.h"

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
