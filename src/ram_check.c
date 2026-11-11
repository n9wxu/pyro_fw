/*
 * See ram_check.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ram_check.h"
#include <string.h>

bool ram_check_step(ram_check_t *c, const uint8_t *ram, const uint8_t *image, uint32_t len, uint32_t piece) {
    if (len == 0 || c->differs)
        return !c->differs;
    if (c->at >= len)
        c->at = 0;
    uint32_t n = len - c->at < piece ? len - c->at : piece;
    if (memcmp(ram + c->at, image + c->at, n) != 0)
        c->differs = true;
    c->at += n;
    if (c->at >= len) {
        c->at = 0;
        c->sweeps++;
    }
    return !c->differs;
}
