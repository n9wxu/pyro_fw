/*
 * Pin assignment: the rules, the file format, and nothing else.
 *
 * Deliberately free of hardware calls so the release matrix can be tested on
 * the host. Applying an assignment is somebody else's job; deciding whether
 * it is legal is this file's.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pin_assign.h"
#include "ini_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Role names, one place ────────────────────────────────────────
 *
 * Served to the web UI through /api/pins/caps, so app.js renders the roles
 * this firmware understands rather than a copy that drifts from them. */
static const struct {
    const char *name;
    uint8_t role;
    uint32_t needs; /* the capability bit the pin must carry */
} role_table[] = {
    {"off", LUA_ROLE_OFF, 0},
    {"out", LUA_ROLE_OUT, FN_DIGITAL},
    {"pwm", LUA_ROLE_PWM, FN_PWM},
    {"in", LUA_ROLE_IN, FN_DIGITAL},
    {"tx", LUA_ROLE_TX, FN_SERIAL},
    {"rx", LUA_ROLE_RX, FN_SERIAL},
    {"pixel", LUA_ROLE_PIXEL, FN_PIXEL},
    {"bridge", LUA_ROLE_BRIDGE, FN_BRIDGE},
};
#define ROLE_COUNT ((int)(sizeof(role_table) / sizeof(role_table[0])))

static uint8_t role_from_name(const char *s) {
    for (int i = 0; i < ROLE_COUNT; i++) {
        if (strcmp(role_table[i].name, s) == 0) {
            return role_table[i].role;
        }
    }
    return LUA_ROLE_OFF;
}

static const char *role_to_name(uint8_t r) {
    for (int i = 0; i < ROLE_COUNT; i++) {
        if (role_table[i].role == r) {
            return role_table[i].name;
        }
    }
    return "off";
}

static uint32_t role_needs(uint8_t r) {
    for (int i = 0; i < ROLE_COUNT; i++) {
        if (role_table[i].role == r) {
            return role_table[i].needs;
        }
    }
    return 0;
}

const char *pin_assign_role_name_of(uint8_t role) {
    return role_to_name(role);
}

int pin_assign_role_count(void) {
    return ROLE_COUNT;
}

const char *pin_assign_role_name(int idx) {
    return (idx >= 0 && idx < ROLE_COUNT) ? role_table[idx].name : "off";
}

uint32_t pin_assign_role_needs(int idx) {
    return (idx >= 0 && idx < ROLE_COUNT) ? role_table[idx].needs : 0;
}

const char *pin_assign_strerror(pin_err_t e) {
    switch (e) {
    case PIN_OK:
        return "ok";
    case PIN_ERR_UNKNOWN_PIN:
        return "this board does not offer that pin";
    case PIN_ERR_NOT_CAPABLE:
        return "the pin cannot take that role";
    case PIN_ERR_PYRO_RETAINED:
        return "release the pyro channel first";
    case PIN_ERR_COMMON_HELD:
        return "the other pyro channel still needs the common";
    case PIN_ERR_BRIDGE_UNSUPPORTED:
        return "this board cannot make a half-bridge";
    case PIN_ERR_BRIDGE_INCOMPLETE:
        return "a bridge needs one channel side and the common";
    case PIN_ERR_BUZZER_NOT_CAPABLE:
        return "that pad cannot drive a buzzer";
    case PIN_ERR_BUZZER_BUSY:
        return "that pad is already doing something else";
    case PIN_ERR_DUPLICATE_NAME:
        return "two resources share one name";
    case PIN_ERR_GT_NOT_CAPABLE:
        return "that pad cannot read the ground test switch";
    case PIN_ERR_GT_BUSY:
        return "that pad is already doing something else";
    case PIN_ERR_GT_INCOMPLETE:
        return "the ground test switch needs its pads: one to ground, or two different pads";
    }
    return "invalid";
}

/* The board as designed: every pyro function retained, no pin assigned to Lua.
 *
 * Zero IS that configuration -- pyro1_released and pyro2_released are false
 * and every role is LUA_ROLE_OFF -- and saying so here is the point, because
 * it is a contract rather than an accident of the struct layout. A board that
 * has never been configured must come up doing what its schematic says it
 * does, with nothing handed to a script. */
void pin_assign_defaults(pin_assign_t *a) {
    memset(a, 0, sizeof(*a));
    /* Not memset's 0, which is GPIO0 -- the telemetry UART on every board
     * here. The default has to mean "wherever the board put it". */
    a->buzzer_pin = PIN_BUZZER_BOARD;
    a->gt_pin = PIN_GT_UNSET;
    a->gt_drive_pin = PIN_GT_UNSET;
}

/* Whether this pad carries the ground test switch. */
static bool is_gt_pad(const pin_assign_t *a, uint8_t pin) {
    if (!a || a->gt_wiring == GT_WIRING_NONE)
        return false;
    return pin == a->gt_pin || (a->gt_wiring == GT_WIRING_PAIR && pin == a->gt_drive_pin);
}

/* The board's own buzzer pad, or PIN_BUZZER_BOARD when it fits none. */
static uint8_t board_buzzer_pad(void) {
    int n = 0;
    const pin_cap_t *t = pin_caps_table(&n);
    for (int i = 0; i < n; i++) {
        if (t[i].functions & FN_BUZZER) {
            return t[i].pin;
        }
    }
    return PIN_BUZZER_BOARD;
}

uint8_t pin_assign_buzzer_pin(const pin_assign_t *a) {
    if (a && a->buzzer_pin != PIN_BUZZER_BOARD) {
        return a->buzzer_pin;
    }
    return board_buzzer_pad();
}

/* Is the flight software still holding this pin?
 *
 * A per-channel element is held until its own channel is released. The common
 * is held until BOTH are, because until then it is half of the retained
 * channel's firing path. Everything else the board reserves -- the sensor
 * bus, the buzzer, the telemetry UART, the LED, the sense pads -- is held
 * unconditionally at this phase. */
bool pin_assign_is_reserved(const pin_assign_t *a, uint8_t pin) {
    const pin_cap_t *c = pin_caps_find(pin);

    /* [PIN-BUZZ-02] The pad driving the buzzer is the flight software's,
     * the board's own or one the operator moved it to; once it has moved,
     * the board's own pad is free for Lua. */
    if (pin_assign_buzzer_pin(a) == pin || is_gt_pad(a, pin)) {
        return true;
    }
    if (c && (c->functions & FN_BUZZER) && a && a->buzzer_pin != PIN_BUZZER_BOARD) {
        return false;
    }

    if (!c || !(c->functions & FN_BOARD_RESERVED)) {
        return false;
    }
    switch (c->group) {
    case PG_CH1:
        return !a->pyro1_released;
    case PG_CH2:
        return !a->pyro2_released;
    case PG_COMMON:
        return !(a->pyro1_released && a->pyro2_released);
    case PG_NONE:
    default:
        return true;
    }
}

static pin_verdict_t ok(void) {
    pin_verdict_t v = {PIN_OK, 0, "ok"};
    return v;
}

static pin_verdict_t fail(pin_err_t e, uint8_t pin) {
    pin_verdict_t v = {e, pin, pin_assign_strerror(e)};
    return v;
}

/* A buzzer is a square wave on a plain output, so FN_DIGITAL is the real
 * requirement; FN_BUZZER additionally means the board already wired one. */
static pin_verdict_t check_buzzer(const pin_assign_t *a) {
    if (a->buzzer_pin == PIN_BUZZER_BOARD) {
        return ok();
    }
    const pin_cap_t *c = pin_caps_find(a->buzzer_pin);
    if (!c) {
        return fail(PIN_ERR_UNKNOWN_PIN, a->buzzer_pin);
    }
    if (!(c->functions & (FN_BUZZER | FN_DIGITAL))) {
        return fail(PIN_ERR_BUZZER_NOT_CAPABLE, a->buzzer_pin);
    }
    /* [PIN-BUZZ-02] A pad with a Lua role cannot also be the buzzer. Not
     * is_reserved(), which answers true FOR the buzzer pad itself. */
    if (a->buzzer_pin < PIN_ASSIGN_MAX_GPIO && a->role[a->buzzer_pin] != LUA_ROLE_OFF) {
        return fail(PIN_ERR_BUZZER_BUSY, a->buzzer_pin);
    }
    if (c->group != PG_NONE) {
        bool released = (c->group == PG_CH1)   ? a->pyro1_released
                        : (c->group == PG_CH2) ? a->pyro2_released
                                               : (a->pyro1_released && a->pyro2_released);
        if (!released) {
            return fail(PIN_ERR_PYRO_RETAINED, a->buzzer_pin);
        }
    }
    /* Everything else the board reserves -- the sensor bus, the UART, the LED
     * -- stays reserved. Only a pad carrying FN_BUZZER is exempt, because that
     * pad IS a buzzer pad. */
    if ((c->functions & FN_BOARD_RESERVED) && !(c->functions & FN_BUZZER) && c->group == PG_NONE) {
        return fail(PIN_ERR_BUZZER_BUSY, a->buzzer_pin);
    }
    return ok();
}

/* One pad of the ground test switch: a plain digital pad that nothing else
 * has -- no script, not the buzzer, nothing the board holds. */
static pin_verdict_t check_gt_pad(const pin_assign_t *a, uint8_t pin) {
    if (pin == PIN_GT_UNSET) {
        return fail(PIN_ERR_GT_INCOMPLETE, 0);
    }
    const pin_cap_t *c = pin_caps_find(pin);
    if (!c) {
        return fail(PIN_ERR_UNKNOWN_PIN, pin);
    }
    if (!(c->functions & FN_DIGITAL)) {
        return fail(PIN_ERR_GT_NOT_CAPABLE, pin);
    }
    if (pin < PIN_ASSIGN_MAX_GPIO && a->role[pin] != LUA_ROLE_OFF) {
        return fail(PIN_ERR_GT_BUSY, pin);
    }
    if (pin_assign_buzzer_pin(a) == pin) {
        return fail(PIN_ERR_GT_BUSY, pin);
    }
    if (c->group != PG_NONE) {
        bool released = (c->group == PG_CH1)   ? a->pyro1_released
                        : (c->group == PG_CH2) ? a->pyro2_released
                                               : (a->pyro1_released && a->pyro2_released);
        if (!released) {
            return fail(PIN_ERR_PYRO_RETAINED, pin);
        }
    } else if (c->functions & FN_BOARD_RESERVED) {
        return fail(PIN_ERR_GT_BUSY, pin);
    }
    return ok();
}

static pin_verdict_t check_ground_test(const pin_assign_t *a) {
    if (a->gt_wiring == GT_WIRING_NONE) {
        return ok();
    }
    pin_verdict_t v = check_gt_pad(a, a->gt_pin);
    if (v.err != PIN_OK || a->gt_wiring != GT_WIRING_PAIR) {
        return v;
    }
    if (a->gt_drive_pin == a->gt_pin) {
        return fail(PIN_ERR_GT_INCOMPLETE, a->gt_pin);
    }
    return check_gt_pad(a, a->gt_drive_pin);
}

pin_verdict_t pin_assign_validate(const pin_assign_t *a) {
    int bridge_channel = 0;
    int bridge_common = 0;

    pin_verdict_t bz = check_buzzer(a);
    if (bz.err != PIN_OK) {
        return bz;
    }
    pin_verdict_t gt = check_ground_test(a);
    if (gt.err != PIN_OK) {
        return gt;
    }

    for (uint8_t pin = 0; pin < PIN_ASSIGN_MAX_GPIO; pin++) {
        uint8_t role = a->role[pin];
        if (role == LUA_ROLE_OFF) {
            continue;
        }

        const pin_cap_t *c = pin_caps_find(pin);
        if (!c) {
            return fail(PIN_ERR_UNKNOWN_PIN, pin);
        }

        /* The board has to support the role at all. */
        uint32_t needs = role_needs(role);
        if (needs && !(c->functions & needs)) {
            return fail(PIN_ERR_NOT_CAPABLE, pin);
        }

        /* And the flight software has to have let go of the pin. The two
         * pyro cases are reported separately because the fix differs: one
         * asks the operator to release a channel, the other tells them why
         * releasing one was not enough. */
        if (pin_assign_is_reserved(a, pin)) {
            if (c->group == PG_COMMON) {
                return fail(PIN_ERR_COMMON_HELD, pin);
            }
            if (c->group == PG_CH1 || c->group == PG_CH2) {
                return fail(PIN_ERR_PYRO_RETAINED, pin);
            }
            return fail(PIN_ERR_NOT_CAPABLE, pin);
        }

        if (role == LUA_ROLE_BRIDGE) {
            if (!pin_caps_bridge_possible()) {
                return fail(PIN_ERR_BRIDGE_UNSUPPORTED, pin);
            }
            if (c->group == PG_COMMON) {
                bridge_common++;
            } else {
                bridge_channel++;
            }
        }

        /* Names address resources from Lua, so two resources cannot share
         * one: find_output() and friends resolve the first match and the
         * second becomes unreachable. */
        if (a->name[pin][0]) {
            for (uint8_t other = 0; other < pin; other++) {
                if (a->role[other] != LUA_ROLE_OFF && strcmp(a->name[pin], a->name[other]) == 0) {
                    return fail(PIN_ERR_DUPLICATE_NAME, pin);
                }
            }
        }
    }

    /* A bridge is a pair. One half on its own drives nothing, and leaving it
     * half-configured would look like it should. */
    if (bridge_channel || bridge_common) {
        if (bridge_channel != 1 || bridge_common != 1) {
            return fail(PIN_ERR_BRIDGE_INCOMPLETE, 0);
        }
    }

    return ok();
}

/* ── pins.ini ─────────────────────────────────────────────────────
 *
 *   [pins]
 *   pyro1_released=true
 *   p9_role=bridge
 *   p9_name=motor
 *
 * config.ini's tokenizer, and its rule for unknown keys (CFG-08): an older
 * firmware must survive a newer file. */

static bool parse_bool(const char *v) {
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

/* A pad number, or `unset` for anything that is not one: never atoi()'s 0,
 * which is a real GPIO, and never a number wrapped into one. A number no pad
 * has is kept, for pin_assign_validate() to refuse. */
static uint8_t parse_pad(const char *v, uint8_t unset) {
    if (v[0] < '0' || v[0] > '9')
        return unset;
    char *end;
    unsigned long p = strtoul(v, &end, 10);
    return (*end == '\0' && p < unset) ? (uint8_t)p : unset;
}

static void parse_pin_key(pin_assign_t *a, const char *key, const char *val) {
    char *suffix;
    unsigned long pin = strtoul(key + 1, &suffix, 10);
    if (suffix == key + 1 || pin >= PIN_ASSIGN_MAX_GPIO)
        return;
    if (strcmp(suffix, "_role") == 0) {
        a->role[pin] = role_from_name(val);
    } else if (strcmp(suffix, "_name") == 0) {
        strncpy(a->name[pin], val, LUA_NAME_MAX - 1);
        a->name[pin][LUA_NAME_MAX - 1] = '\0';
    }
}

static void parse_pair(const char *key, const char *val, void *ctx) {
    pin_assign_t *a = ctx;
    if (strcmp(key, "buzzer_pin") == 0) {
        /* "board" is the default and the only non-numeric value. */
        a->buzzer_pin = parse_pad(val, PIN_BUZZER_BOARD);
    } else if (strcmp(key, "ground_test") == 0) {
        /* An unknown wiring is none, never a guess. */
        a->gt_wiring = strcmp(val, "ground") == 0 ? GT_WIRING_GROUND
                       : strcmp(val, "pair") == 0 ? GT_WIRING_PAIR
                                                  : GT_WIRING_NONE;
    } else if (strcmp(key, "ground_test_pin") == 0) {
        a->gt_pin = parse_pad(val, PIN_GT_UNSET);
    } else if (strcmp(key, "ground_test_drive_pin") == 0) {
        a->gt_drive_pin = parse_pad(val, PIN_GT_UNSET);
    } else if (strcmp(key, "pyro1_released") == 0) {
        a->pyro1_released = parse_bool(val);
    } else if (strcmp(key, "pyro2_released") == 0) {
        a->pyro2_released = parse_bool(val);
    } else if (key[0] == 'p' && key[1] >= '0' && key[1] <= '9') {
        parse_pin_key(a, key, val);
    }
}

void pin_assign_parse_ini(char *buf, pin_assign_t *a) {
    ini_for_each(buf, parse_pair, a);
}

int pin_assign_serialize_ini(const pin_assign_t *a, char *buf, int max_len) {
    int pos = 0;

/* Same guard as config_serialize_ini(): snprintf returns what it WOULD have
 * written, so an unguarded accumulation hands the next call a negative size. */
#define APPEND(fmt, ...)                                                                                               \
    do {                                                                                                               \
        if (pos >= max_len)                                                                                            \
            return -1;                                                                                                 \
        int n = snprintf(buf + pos, (size_t)(max_len - pos), fmt, ##__VA_ARGS__);                                      \
        if (n < 0 || n >= max_len - pos)                                                                               \
            return -1;                                                                                                 \
        pos += n;                                                                                                      \
    } while (0)

    APPEND("[pins]\r\n");
    APPEND("pyro1_released=%s\r\n", a->pyro1_released ? "true" : "false");
    APPEND("pyro2_released=%s\r\n", a->pyro2_released ? "true" : "false");
    if (a->buzzer_pin == PIN_BUZZER_BOARD) {
        APPEND("buzzer_pin=board\r\n");
    } else {
        APPEND("buzzer_pin=%u\r\n", (unsigned)a->buzzer_pin);
    }
    APPEND("ground_test=%s\r\n", a->gt_wiring == GT_WIRING_GROUND ? "ground"
                                 : a->gt_wiring == GT_WIRING_PAIR ? "pair"
                                                               : "none");
    if (a->gt_pin != PIN_GT_UNSET) {
        APPEND("ground_test_pin=%u\r\n", (unsigned)a->gt_pin);
    }
    if (a->gt_drive_pin != PIN_GT_UNSET) {
        APPEND("ground_test_drive_pin=%u\r\n", (unsigned)a->gt_drive_pin);
    }

    /* Only assigned pins, so the file stays about as long as the
     * configuration is complicated. */
    for (uint8_t pin = 0; pin < PIN_ASSIGN_MAX_GPIO; pin++) {
        if (a->role[pin] == LUA_ROLE_OFF) {
            continue;
        }
        APPEND("p%u_role=%s\r\n", (unsigned)pin, role_to_name(a->role[pin]));
        if (a->name[pin][0]) {
            APPEND("p%u_name=%s\r\n", (unsigned)pin, a->name[pin]);
        }
    }
#undef APPEND

    return pos;
}
