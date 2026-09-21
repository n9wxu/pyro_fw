/*
 * Pyro backend — REFERENCE BOARD (template).
 *
 * Implements src/pyro.h. As shipped this is a safe stub: it reports no
 * continuity and refuses to fire, so a board brought up from this template
 * cannot energise anything before you have written the real driver.
 *
 * Compare boards/mk1b/pyro_board.c (AP2192 high-side switches, a simple
 * common-enable design) against boards/mk1c/pyro_board.c (TPS259570 eFuse
 * with a software charge-pump arm) -- the two share nothing but this
 * interface, which is the point.
 *
 * SAFETY, if your board can fire a pyrotechnic device:
 *   - Drive every output inactive in pyro_init(), and from
 *     board_early_init() before any slow initialisation runs.
 *   - Report good == false until continuity is genuinely proven. The
 *     flight logic gates firing on it, which is a second barrier behind
 *     whatever pyro_fire() itself checks.
 *   - Keep the set of functions that can assert a firing output as small
 *     as possible, ideally one, so it can be reviewed as a unit.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pyro.h"
#include "board_pins.h"

extern void hal_telemetry_send(const char *sentence);

void pyro_init(void) {
    /* TODO: claim your pyro pins and drive them inactive. */
    hal_telemetry_send("!PYRO reference stub: firing not implemented\r\n");
}

/* TODO: perform ONE stimulus event here and latch both channels. The
 * stimulus is shared on both existing boards, so doing it per channel would
 * double the current through the bridgewire on every routine check. */
void pyro_sample(void) {}

void pyro_get(uint8_t channel, pyro_continuity_t *out) {
    /* TODO: report the RAW reading in raw_adc as well as the booleans -- a
     * degraded match sits between the thresholds and only the raw number
     * shows it. Report good == false until continuity is genuinely proven. */
    if (channel != 1 && channel != 2)
        return;
    out->raw_adc = 0;
    out->good = false;
    out->open = true;
    out->shorted = false;
}

void pyro_fire(uint8_t channel) {
    (void)channel;
    /* TODO: implement, or leave refusing. */
    hal_telemetry_send("!PYRO FIRE REFUSED: not implemented\r\n");
}

void pyro_update(uint32_t now_ms) {
    /* Called every main-loop iteration. Drive any non-blocking state
     * machine here: duty-cycled measurement, fire-pulse timing, drain
     * monitoring. Must not block. */
    (void)now_ms;
}

bool pyro_is_firing(void) {
    return false;
}

bool pyro_fault(uint8_t channel) {
    (void)channel;
    return false;
}
