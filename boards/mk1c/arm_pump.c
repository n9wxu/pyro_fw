/*
 * SPDX-License-Identifier: MIT
 */
#include "arm_pump.h"
#include "board_pins.h"
#include "arm_pump.pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#define ARM_PIO pio0

static uint arm_sm;
static uint arm_offset;
static bool arm_pio_loaded;

void arm_pump_start(void) {
    if (!arm_pio_loaded) {
        arm_offset = pio_add_program(ARM_PIO, &arm_pump_program);
        arm_sm = (uint)pio_claim_unused_sm(ARM_PIO, true);
        arm_pio_loaded = true;
    }
    /* 125 MHz / (50 clocks per period / period_us * 1e6) */
    float clkdiv = (float)clock_get_hz(clk_sys) / (50.0f * (1000000.0f / (float)ARM_PUMP_PERIOD_US));
    arm_pump_program_init(ARM_PIO, arm_sm, arm_offset, BOARD_PIN_ARM_TOGGLE, clkdiv);
}

void arm_pump_stop(void) {
    if (!arm_pio_loaded)
        return;
    pio_sm_set_enabled(ARM_PIO, arm_sm, false);
    pio_sm_clear_fifos(ARM_PIO, arm_sm);
    /* Hand the pad back to SIO and drive it low, so nothing can toggle it. */
    gpio_init(BOARD_PIN_ARM_TOGGLE);
    gpio_put(BOARD_PIN_ARM_TOGGLE, 0);
    gpio_set_dir(BOARD_PIN_ARM_TOGGLE, GPIO_OUT);
    gpio_put(BOARD_PIN_ARM_TOGGLE, 0);
}
