/*
 * Loading and storing the pin assignment. See pin_store.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pin_store.h"
#include "hal.h"
#include "pin_caps.h"
#include <stdio.h>
#include <string.h>

static pin_assign_t live_pins;
static char pins_load_reason[96];

const pin_assign_t *pin_store_current(void) {
    return &live_pins;
}

const char *pin_store_reason(void) {
    return pins_load_reason;
}

void pin_store_load(char *reason, int reason_len) {
    pins_load_reason[0] = '\0';

    pin_assign_t defaults;
    pin_assign_defaults(&defaults);
    live_pins = defaults;

    /* Every fallback is said, so an empty reason means a clean load. */
    char buf[PIN_STORE_MAX];
    int n = hal_fs_read_file(PIN_STORE_PATH, buf, (int)sizeof(buf) - 1);
    if (n == HAL_FS_NOENT) {
        char out[PIN_STORE_MAX];
        int w = pin_assign_serialize_ini(&defaults, out, (int)sizeof(out));
        bool wrote = (w > 0) && (hal_fs_write_file(PIN_STORE_PATH, out, w) == 0);
        (void)snprintf(pins_load_reason, sizeof(pins_load_reason), "no %s; board defaults%s", PIN_STORE_PATH,
                       wrote ? " written" : " (could not write)");
    } else if ((n < 0) || (n >= ((int)sizeof(buf) - 1))) {
        /* Unreadable, or longer than it can be: the file is kept for
         * the next boot, which may read it. */
        (void)snprintf(pins_load_reason, sizeof(pins_load_reason), "%s %s (%d); board defaults, file kept", PIN_STORE_PATH,
                       (n < 0) ? "unreadable" : "too long", n);
    } else {
        buf[n] = '\0';
        pin_assign_t from_file;
        pin_assign_defaults(&from_file);
        pin_assign_parse_ini(buf, &from_file);

        pin_verdict_t v = pin_assign_validate(&from_file);
        if (v.err == PIN_OK) {
            live_pins = from_file;
        } else {
            /* Not the good rows: the defaults keep the pyro channels retained. */
            (void)snprintf(pins_load_reason, sizeof(pins_load_reason), "pins.ini rejected: %s (pin %u)", pin_assign_strerror(v.err),
                           (unsigned)v.pin);
        }
    }

    if ((reason != NULL) && (reason_len > 0)) {
        (void)snprintf(reason, (size_t)reason_len, "%s", pins_load_reason);
    }
}

pin_verdict_t pin_store_save(const pin_assign_t *a) {
    pin_verdict_t v = pin_assign_validate(a);
    if (v.err != PIN_OK) {
        return v;
    }

    char buf[PIN_STORE_MAX];
    int n = pin_assign_serialize_ini(a, buf, (int)sizeof(buf));
    if (n <= 0) {
        pin_verdict_t bad = {PIN_ERR_NOT_CAPABLE, 0, "assignment does not fit pins.ini"};
        return bad;
    }
    if (hal_fs_write_file(PIN_STORE_PATH, buf, n) != 0) {
        pin_verdict_t bad = {PIN_ERR_NOT_CAPABLE, 0, "could not write pins.ini"};
        return bad;
    }

    live_pins = *a;
    return v;
}

uint32_t pin_store_pyro_pads(uint8_t channel) {
    pin_group_t want = (channel == 1u) ? PG_CH1 : ((channel == 2u) ? PG_CH2 : PG_NONE);
    if (want == PG_NONE) {
        return PAD_NONE;
    }

    int n = 0;
    const pin_cap_t *caps = pin_caps_table(&n);
    uint32_t pads = PAD_NONE;
    uint32_t common = PAD_NONE;
    for (int i = 0; i < n; i++) {
        if (caps[i].pin >= PAD_CLAIM_MAX_GPIO) {
            continue;
        }
        if (caps[i].group == want) {
            pads |= PAD(caps[i].pin);
        } else if (caps[i].group == PG_COMMON) {
            common |= PAD(caps[i].pin);
        } else {
            /* the other channel's pad */
        }
    }
    /* The common only counts as this channel's while the channel exists --
     * an empty channel must not claim it and lock the other one out. */
    return (pads != 0u) ? (pads | common) : PAD_NONE;
}

/* One write per pad. Nothing here compares an owner against another; the
 * exclusivity is that there is one slot and one assignment to it. */
void pin_store_claim_pads(void) {
    pad_claim_reset();

    int n = 0;
    const pin_cap_t *caps = pin_caps_table(&n);
    for (int i = 0; i < n; i++) {
        uint8_t pin = caps[i].pin;
        if (pin >= PAD_CLAIM_MAX_GPIO) {
            continue;
        }
        pad_claim_take(PAD(pin), pin_assign_is_reserved(&live_pins, pin) ? PAD_FLIGHT : PAD_LUA);
    }
}

bool pin_store_has_buzzer(void) {
    return pin_assign_buzzer_pin(&live_pins) != PIN_BUZZER_BOARD;
}

bool pin_store_owns(uint8_t pin) {
    return pin_assign_is_reserved(&live_pins, pin);
}

bool pin_store_bridge(uint8_t *channel_pin, uint8_t *common_pin, const char **name) {
    int ch = -1;
    int common = -1;
    for (uint8_t pin = 0; (int)pin < PIN_ASSIGN_MAX_GPIO; pin++) {
        if ((lua_role_t)live_pins.role[pin] != LUA_ROLE_BRIDGE) {
            continue;
        }
        const pin_cap_t *c = pin_caps_find(pin);
        if (c == NULL) {
            continue;
        }
        if (c->group == PG_COMMON) {
            common = pin;
        } else {
            ch = pin;
        }
    }
    /* Validation already rejects a half-configured bridge, so this is a
     * belt-and-braces check on a path that drives a FET gate. */
    if ((ch < 0) || (common < 0)) {
        return false;
    }
    *channel_pin = (uint8_t)ch;
    *common_pin = (uint8_t)common;
    *name = live_pins.name[ch];
    return true;
}

/* Every pad Lua drives as plain GPIO: the board's own, plus any pyro pad the
 * configuration released.
 *
 * A released pad is an ordinary output or input -- one released high side is
 * a single high-side switch, and with both channels released all three
 * elements are available. Only LUA_ROLE_BRIDGE is excluded, because a bridge
 * is not a GPIO: it consumes two pads and goes through
 * lua_plat_configure_bridge() and the pyro PIO instead.
 *
 * Each entry carries its own pin, so nothing here is positional. */
int pin_store_lua_pins(lua_pin_cfg_t *out, int max) {
    static const uint8_t lua_pins[] = LUA_PIN_LIST;
    int n = 0;

    for (unsigned i = 0; (i < (sizeof(lua_pins) / sizeof(lua_pins[0]))) && (n < max); i++) {
        uint8_t pin = lua_pins[i];
        out[n].pin = pin;
        out[n].role = (lua_role_t)live_pins.role[pin];
        out[n].name = live_pins.name[pin];
        n++;
    }

    for (uint8_t pin = 0; ((int)pin < PIN_ASSIGN_MAX_GPIO) && (n < max); pin++) {
        if (((lua_role_t)live_pins.role[pin] == LUA_ROLE_OFF) || ((lua_role_t)live_pins.role[pin] == LUA_ROLE_BRIDGE)) {
            continue;
        }
        if (pin_assign_is_reserved(&live_pins, pin)) {
            continue; /* still the flight software's */
        }
        const pin_cap_t *c = pin_caps_find(pin);
        if ((c == NULL) || ((c->functions & FN_BOARD_RESERVED) == 0u)) {
            continue; /* a default pad, already added above */
        }
        out[n].pin = pin;
        out[n].role = (lua_role_t)live_pins.role[pin];
        out[n].name = live_pins.name[pin];
        n++;
    }
    return n;
}
