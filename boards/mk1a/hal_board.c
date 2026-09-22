/*
 * Board support — Pyro MK1A (bare RP2040).
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
#include "pico/binary_info.h"

extern void pyro_safe_all_outputs(void); /* boards/mk1a/pyro_board.c */

void board_early_init(void) {
    /* Drive every pyro output inactive at the earliest opportunity, before
     * anything slow (USB, networking, the filesystem) runs. The RP2040 pad
     * reset state (input with pull-down) already achieves this before any
     * code runs, so this asserts the safe state rather than establishing it.
     *
     * No buzzer on this board, so unlike MK1B/MK1C there is nothing to
     * silence here. */
    pyro_safe_all_outputs();
}

void board_hw_init(void) {
    /* Heartbeat LED (D3/D4). Start ON so a lit LED confirms the correct
     * GPIO. Must run after the TinyUSB BSP's board_init(), which also drives
     * this pin -- on MK1A that is harmless, because GPIO25 really is the LED
     * here. */
    gpio_init(BOARD_PIN_LED);
    gpio_set_dir(BOARD_PIN_LED, GPIO_OUT);
    gpio_put(BOARD_PIN_LED, 1);

    /* Telemetry UART pins; hal_common calls uart_init() on the instance. */
    gpio_set_function(BOARD_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(BOARD_PIN_UART_RX, GPIO_FUNC_UART);

    /* The two sense channels are claimed by pyro_init(), which also calls
     * adc_init(). Nothing to do here. */
}

void board_led_set(bool on) {
    gpio_put(BOARD_PIN_LED, on);
}

void board_led_toggle(void) {
    gpio_xor_mask(1u << BOARD_PIN_LED);
}

/* No buzzer is fitted. These are no-ops rather than absent, because
 * board_if.h is a contract a board implements as-is. */
void board_buzzer_init(void) {}
void board_buzzer_on(void) {}
void board_buzzer_off(void) {}

bool board_pyro_raw(board_pyro_raw_t *out) {
    /* MK1A has no bus-level analog sensing -- only the two per-channel sense
     * taps -- so there is nothing to fill the bus/bias fields with. Reporting
     * false leaves them out of /api/status entirely, which is better than
     * publishing zeros that read as "bus cold, all healthy". */
    (void)out;
    return false;
}

bool board_pyro_wave_request(int mode) {
    (void)mode;
    return false; /* no high-speed capture on this board */
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

/* ── Identification for picotool ──────────────────────────────────
 *
 * `picotool info -a` prints these, so an attached board says what it is
 * before anyone reaches for `picotool load`. The pyro pins are the useful
 * part: they are what actually differs between boards, and getting them
 * wrong is how an image ends up driving a bias injector as an LED. */
bi_decl(bi_1pin_with_name(BOARD_PIN_LED, "LED (D3/D4)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_UART_TX, "UART0 TX -> J6", BOARD_PIN_UART_RX, "UART0 RX <- J6"));
bi_decl(bi_2pins_with_names(BOARD_PIN_I2C_SDA, "I2C0 SDA (BMP280)", BOARD_PIN_I2C_SCL, "I2C0 SCL (BMP280)"));
bi_decl(bi_3pins_with_names(BOARD_PIN_FIRE1, "PYRO FIRE1 (Q6 high side)", BOARD_PIN_PYRO_LOW,
                            "PYRO shared low side (Q2)", BOARD_PIN_FIRE2, "PYRO FIRE2 (Q1 high side)"));
bi_decl(bi_2pins_with_names(BOARD_PIN_PYRO1_SENSE, "PYRO sense 1 (ADC0)", BOARD_PIN_PYRO2_SENSE,
                            "PYRO sense 2 (ADC1)"));
