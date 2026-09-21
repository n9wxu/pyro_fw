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

void board_buzzer_init(void) {
    gpio_init(BOARD_PIN_BUZZER);
    gpio_set_dir(BOARD_PIN_BUZZER, GPIO_OUT);
    gpio_put(BOARD_PIN_BUZZER, 0);
}

void board_buzzer_on(void) {
    gpio_put(BOARD_PIN_BUZZER, 1);
}

void board_buzzer_off(void) {
    gpio_put(BOARD_PIN_BUZZER, 0);
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
