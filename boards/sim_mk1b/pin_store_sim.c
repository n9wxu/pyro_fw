/*
 * The simulated MK1B releases nothing to Lua, which it does not run: the
 * flight software owns every pad.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pin_store.h"

bool pin_store_owns(uint8_t pin) {
    (void)pin;
    return true;
}
