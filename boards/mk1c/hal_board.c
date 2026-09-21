/*
 * Board support — Pyro MK1C (bare RP2040).
 *
 * Implements src/board_if.h. Everything board-independent lives in
 * src/hal_common/.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_if.h"
#include "board_pins.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

extern void pyro_safe_all_outputs(void); /* boards/mk1c/pyro_board.c */

void board_early_init(void) {
    /* Silence the buzzer before any slow init (USB, networking) can leave
     * the pin floating and produce a spurious tone at power-on. */
    board_buzzer_init();

    /* Drive every pyro output inactive at the earliest opportunity. The
     * RP2040 pad reset state (input with pull-down) already achieves this
     * before any code runs, so this asserts the safe state rather than
     * establishing it. */
    pyro_safe_all_outputs();
}

void board_hw_init(void) {
    /* Heartbeat LED (D1). Start ON so a lit LED confirms the correct GPIO.
     * Must run after the TinyUSB BSP's board_init(). */
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1);

    /* Telemetry UART pins; hal_common calls uart_init() on the instance. */
    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);

    /* The four sense channels are claimed by pyro_init(), which also calls
     * adc_init(). Nothing to do here. */
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

bool board_pyro_raw(board_pyro_raw_t *out) {
    extern bool pyro_raw_sense(board_pyro_raw_t * out);
    return pyro_raw_sense(out);
}

bool board_pyro_wave_request(int mode) {
    extern bool pyro_wave_request(int mode);
    return pyro_wave_request(mode);
}

int board_pyro_wave_state(void) {
    extern int pyro_wave_state(void);
    return pyro_wave_state();
}

uart_inst_t *board_uart(void) {
    return BOARD_UART_INST;
}

uint board_uart_irq(void) {
    return BOARD_UART_IRQ_NUM;
}
