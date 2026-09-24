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

/* ── Migration from the lua_p18..p21 keys ─────────────────────────
 *
 * Those keys were MK1C-shaped: lua_app.c built a four-entry array and
 * lua_plat_configure() indexed it POSITIONALLY against LUA_PIN_LIST, so on
 * MK1B lua_p18_role actually configured GPIO8. The migration has to preserve
 * that, not the pin numbers the key names imply, or every existing MK1A and
 * MK1B board would come back with its Lua pins moved. */
static uint8_t role_of_legacy(const char *s) {
    if (strcmp(s, "out") == 0)
        return LUA_ROLE_OUT;
    if (strcmp(s, "pwm") == 0)
        return LUA_ROLE_PWM;
    if (strcmp(s, "in") == 0)
        return LUA_ROLE_IN;
    if (strcmp(s, "tx") == 0)
        return LUA_ROLE_TX;
    if (strcmp(s, "rx") == 0)
        return LUA_ROLE_RX;
    if (strcmp(s, "pixel") == 0)
        return LUA_ROLE_PIXEL;
    return LUA_ROLE_OFF;
}

static void migrate_from_config(const config_t *cfg, pin_assign_t *a) {
    static const uint8_t lua_pins[] = LUA_PIN_LIST;
    const struct {
        const char *role;
        const char *name;
    } legacy[4] = {
        {cfg->lua_p18_role, cfg->lua_p18_name},
        {cfg->lua_p19_role, cfg->lua_p19_name},
        {cfg->lua_p20_role, cfg->lua_p20_name},
        {cfg->lua_p21_role, cfg->lua_p21_name},
    };

    for (unsigned i = 0; i < 4 && i < sizeof(lua_pins) / sizeof(lua_pins[0]); i++) {
        uint8_t pin = lua_pins[i];
        if (pin >= PIN_ASSIGN_MAX_GPIO) {
            continue;
        }
        a->role[pin] = role_of_legacy(legacy[i].role);
        strncpy(a->name[pin], legacy[i].name, LUA_NAME_MAX - 1);
        a->name[pin][LUA_NAME_MAX - 1] = '\0';
    }
}

void pin_store_load(const config_t *cfg, char *reason, int reason_len) {
    load_reason[0] = '\0';

    pin_assign_t migrated;
    pin_assign_defaults(&migrated);
    migrate_from_config(cfg, &migrated);

    char buf[PIN_STORE_MAX];
    int n = hal_fs_read_file(PIN_STORE_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        /* No file yet: the board keeps the Lua pins it already had. Said out
         * loud, because an empty reason otherwise reads as "loaded cleanly"
         * and makes a file that was never found look like one that was. */
        live = migrated;
        snprintf(load_reason, sizeof(load_reason), "no %s; using legacy config keys", PIN_STORE_PATH);
    } else {
        buf[n] = '\0';
        pin_assign_t from_file;
        pin_assign_defaults(&from_file);
        pin_assign_parse_ini(buf, &from_file);

        pin_verdict_t v = pin_assign_validate(&from_file);
        if (v.err == PIN_OK) {
            live = from_file;
        } else {
            /* Whole-file rejection. Falling back to the migrated assignment
             * rather than to nothing keeps the board usable and keeps the
             * pyro channels retained, which is the safe direction. */
            live = migrated;
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
