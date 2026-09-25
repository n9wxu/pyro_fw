/*
 * Board support — REFERENCE BOARD (template).
 *
 * Implements src/board_if.h. Copy this file and fill in the bodies; the
 * set of functions is fixed by the contract, so if it compiles and links,
 * src/hal_common has everything it needs.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_if.h"
#include "board_pins.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

void board_early_init(void) {
    /* Runs first in hal_platform_init(), before USB, networking or the
     * filesystem. Put anything that must reach a safe state immediately
     * here -- silencing the buzzer, and driving any pyro output inactive. */
    board_buzzer_init();
    /* TODO: safe any board outputs that can do something dangerous */
}

void board_hw_init(void) {
    /* Runs after the TinyUSB BSP's board_init(), which reinitialises any
     * pin named by PICO_DEFAULT_LED_PIN in your SDK board header. */
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1); /* start ON: a lit LED confirms the GPIO */

    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);

    /* TODO: adc_gpio_init() for any ADC inputs not claimed by pyro_init() */
}

void board_led_set(bool on) {
    gpio_put(BOARD_PIN_LED, on);
}

void board_led_toggle(void) {
    gpio_xor_mask(1u << BOARD_PIN_LED);
}

/* The buzzer pad, runtime rather than compile-time: an operator may move it.
 * The template declares a pad; a real board should check it. */
static uint8_t buzz_pin = BOARD_PIN_BUZZER;

void board_buzzer_set_pin(uint8_t pin) {
    if (pin == buzz_pin) {
        return;
    }
    /* Hand the old pad back as an input first. Leaving it an output would
     * keep it driven after a reassignment, and on a released pyro pad that is
     * a driven pin nobody believes is driven. */
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 0);
        gpio_set_dir(buzz_pin, GPIO_IN);
    }
    buzz_pin = pin;
    board_buzzer_init();
}

void board_buzzer_init(void) {
    if (buzz_pin == BOARD_BUZZER_NO_PIN) {
        return;
    }
    gpio_init(buzz_pin);
    gpio_set_dir(buzz_pin, GPIO_OUT);
    gpio_put(buzz_pin, 0);
}

void board_buzzer_on(void) {
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 1);
    }
}

void board_buzzer_off(void) {
    if (buzz_pin != BOARD_BUZZER_NO_PIN) {
        gpio_put(buzz_pin, 0);
    }
}

/* TODO: report raw sense counts if this board has them. */
bool board_pyro_raw(board_pyro_raw_t *out) {
    (void)out;
    return false;
}

bool board_pyro_wave_request(int mode) {
    (void)mode;
    return false;
}

int board_pyro_wave_state(void) {
    return 0;
}

uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}
