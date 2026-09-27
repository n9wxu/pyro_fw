/*
 * Pyro backend -- REFERENCE BOARD (template). A safe stub: it reports no
 * continuity and refuses to fire, so a board brought up from this template
 * energises nothing before its real backend is written. See
 * THEORY_OF_OPERATION.md "Pyro backend" for what a real one must do.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "board_if.h"
#include "board_pins.h"

extern void hal_telemetry_send(const char *sentence);

void board_early_init(void) {
    /* TODO: drive every pyro output inactive, as boards/mk1c/pyro_board.c does. */
}

void pyro_init(void) {
    /* TODO: claim the pyro pins and drive them inactive. */
    hal_telemetry_send("!PYRO reference stub: firing not implemented\r\n");
}

void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    if (channel != 1 && channel != 2)
        return;
    *out = (pyro_continuity_t){.raw_adc = 0, .good = false, .open = true, .shorted = false};
}

void pyro_fire(uint8_t channel) {
    (void)channel;
    hal_telemetry_send("!PYRO FIRE REFUSED: not implemented\r\n");
}

void pyro_update(uint32_t now_ms) {
    (void)now_ms; /* TODO: the board's non-blocking check and pulse timing */
}

bool pyro_is_firing(void) {
    return false;
}

bool pyro_fault(uint8_t channel) {
    (void)channel;
    return false;
}
