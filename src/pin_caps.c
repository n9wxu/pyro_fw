/*
 * The board's pin capability table, instantiated and checked.
 *
 * A pin with no row here is not assignable to anything: pin_caps_find()
 * returns NULL and every caller treats that as "the board does not offer this
 * pin". MK1C's bias injectors are the worked example -- BIAS_A, BIAS_B and
 * BIAS_BUS are driven by the sense tests and appear in no row, so no
 * configuration can reach them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pin_model.h"
#include "pin_caps.h"

/* Fires every check in PIN_CAPS_ASSERT against this board's rows. */
PIN_CAPS_ASSERT(BOARD_PIN_CAPS)

#define X_ROW(pin, fn, pg) {(pin), (fn), (pg)},
static const pin_cap_t caps[] = {BOARD_PIN_CAPS(X_ROW)};
#undef X_ROW

#define X_COUNT(pin, fn, pg) +1
#define CAP_COUNT (0 BOARD_PIN_CAPS(X_COUNT))

const pin_cap_t *pin_caps_table(int *count) {
    if (count) {
        *count = CAP_COUNT;
    }
    return caps;
}

const pin_cap_t *pin_caps_find(uint8_t pin) {
    for (int i = 0; i < CAP_COUNT; i++) {
        if (caps[i].pin == pin) {
            return &caps[i];
        }
    }
    return NULL;
}

bool pin_caps_is_default_lua(const pin_cap_t *c) {
    return c && (c->functions & FN_LUA_ANY) && !(c->functions & FN_BOARD_RESERVED);
}

/* A bridge needs one per-channel element and the common, and both have to be
 * able to sit at a level. MK1C fails here rather than by topology: its common
 * is the ARM_TOGGLE charge pump, which carries no FN_BRIDGE because holding
 * it at a level does not hold the eFuse on. */
bool pin_caps_bridge_possible(void) {
    bool have_channel = false;
    bool have_common = false;
    for (int i = 0; i < CAP_COUNT; i++) {
        if (!(caps[i].functions & FN_BRIDGE)) {
            continue;
        }
        if (caps[i].group == PG_CH1 || caps[i].group == PG_CH2) {
            have_channel = true;
        } else if (caps[i].group == PG_COMMON) {
            have_common = true;
        }
    }
    return have_channel && have_common;
}

/* LUA_PIN_LIST and the table are two hand-written statements of the same
 * fact, in the same file. This is the cheap check that they agree.
 *
 * Not a _Static_assert because the list cannot be iterated at compile time:
 * an X-macro cannot conditionally emit array elements, so the list stays
 * explicit and the agreement is checked here instead. */
int pin_caps_check_lua_list(void) {
    static const uint8_t listed[] = LUA_PIN_LIST;
    for (unsigned i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
        if (!pin_caps_is_default_lua(pin_caps_find(listed[i]))) {
            return (int)listed[i];
        }
    }
    return -1;
}
