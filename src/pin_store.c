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

static pin_assign_t live;
static char load_reason[96];

const pin_assign_t *pin_store_current(void) {
    return &live;
}

const char *pin_store_reason(void) {
    return load_reason;
}

void pin_store_load(char *reason, int reason_len) {
    load_reason[0] = '\0';

    /* The fallback is the board's own defaults: nothing released, every pyro
     * pad retained. There is no migration from config.ini any more -- the
     * lua_p18..p21 keys are gone, and a board without pins.ini is a board
     * that has not been configured. */
    pin_assign_t defaults;
    pin_assign_defaults(&defaults);

    char buf[PIN_STORE_MAX];
    int n = hal_fs_read_file(PIN_STORE_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        /* Write the defaults out, so the operator has a file to edit rather
         * than an absence to guess at. The board comes up as designed --
         * every pyro function retained, no pin on Lua -- and the Lua tab
         * then shows the pads that are available to assign.
         *
         * At boot the flash window is still open (it starts open at reset and
         * closes at the end of core0's first loop iteration), so this write
         * lands. If it does not, the board runs on the defaults anyway and
         * says so; nothing here is load-bearing.
         *
         * Said out loud either way, because an empty reason reads as "loaded
         * cleanly" and makes a file that was never found look like one that
         * was. */
        live = defaults;
        char out[PIN_STORE_MAX];
        int w = pin_assign_serialize_ini(&defaults, out, (int)sizeof(out));
        bool wrote = (w > 0) && (hal_fs_write_file(PIN_STORE_PATH, out, w) == 0);
        snprintf(load_reason, sizeof(load_reason), "no %s; board defaults%s", PIN_STORE_PATH,
                 wrote ? " written" : " (could not write)");
    } else {
        buf[n] = '\0';
        pin_assign_t from_file;
        pin_assign_defaults(&from_file);
        pin_assign_parse_ini(buf, &from_file);

        pin_verdict_t v = pin_assign_validate(&from_file);
        if (v.err == PIN_OK) {
            live = from_file;
        } else {
            /* Whole-file rejection. Falling back to the defaults rather than
             * to the good rows keeps the pyro channels retained, which is the
             * safe direction. */
            live = defaults;
            snprintf(load_reason, sizeof(load_reason), "pins.ini rejected: %s (pin %u)", pin_assign_strerror(v.err),
                     (unsigned)v.pin);
        }
    }

    if (reason && reason_len > 0) {
        snprintf(reason, (size_t)reason_len, "%s", load_reason);
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

    live = *a;
    return v;
}

uint32_t pin_store_pyro_pads(uint8_t channel) {
    pin_group_t want = (channel == 1) ? PG_CH1 : (channel == 2) ? PG_CH2 : PG_NONE;
    if (want == PG_NONE) {
        return PAD_NONE;
    }

    int n = 0;
    const pin_cap_t *caps = pin_caps_table(&n);
    uint32_t pads = PAD_NONE, common = PAD_NONE;
    for (int i = 0; i < n; i++) {
        if (caps[i].pin >= PAD_CLAIM_MAX_GPIO) {
            continue;
        }
        if (caps[i].group == want) {
            pads |= PAD(caps[i].pin);
        } else if (caps[i].group == PG_COMMON) {
            common |= PAD(caps[i].pin);
        }
    }
    /* The common only counts as this channel's while the channel exists --
     * an empty channel must not claim it and lock the other one out. */
    return pads ? (pads | common) : PAD_NONE;
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
        pad_claim_take(PAD(pin), pin_assign_is_reserved(&live, pin) ? PAD_FLIGHT : PAD_LUA);
    }
}

bool pin_store_has_buzzer(void) {
    return pin_assign_buzzer_pin(&live) != PIN_BUZZER_BOARD;
}

bool pin_store_owns(uint8_t pin) {
    return pin_assign_is_reserved(&live, pin);
}

bool pin_store_bridge(uint8_t *channel_pin, uint8_t *common_pin, const char **name) {
    int ch = -1, common = -1;
    for (uint8_t pin = 0; pin < PIN_ASSIGN_MAX_GPIO; pin++) {
        if (live.role[pin] != LUA_ROLE_BRIDGE) {
            continue;
        }
        const pin_cap_t *c = pin_caps_find(pin);
        if (!c) {
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
    if (ch < 0 || common < 0) {
        return false;
    }
    *channel_pin = (uint8_t)ch;
    *common_pin = (uint8_t)common;
    *name = live.name[ch];
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

    for (unsigned i = 0; i < sizeof(lua_pins) / sizeof(lua_pins[0]) && n < max; i++) {
        uint8_t pin = lua_pins[i];
        out[n].pin = pin;
        out[n].role = (lua_role_t)live.role[pin];
        out[n].name = live.name[pin];
        n++;
    }

    for (uint8_t pin = 0; pin < PIN_ASSIGN_MAX_GPIO && n < max; pin++) {
        if (live.role[pin] == LUA_ROLE_OFF || live.role[pin] == LUA_ROLE_BRIDGE) {
            continue;
        }
        if (pin_assign_is_reserved(&live, pin)) {
            continue; /* still the flight software's */
        }
        const pin_cap_t *c = pin_caps_find(pin);
        if (!c || !(c->functions & FN_BOARD_RESERVED)) {
            continue; /* a default pad, already added above */
        }
        out[n].pin = pin;
        out[n].role = (lua_role_t)live.role[pin];
        out[n].name = live.name[pin];
        n++;
    }
    return n;
}
