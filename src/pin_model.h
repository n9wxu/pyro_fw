/*
 * What a pin may become, and where it sits in a power group.
 *
 * A board declares one row per assignable pin in its own pin_caps.h. This
 * header is the vocabulary those rows are written in, plus the assertions
 * that check them at build time.
 *
 * Configuration chooses an assignment; the board decides which assignments
 * exist. That split is what keeps DECISIONS.md DD-012 substantially intact
 * while letting an operator retask a pin: the pin map still lives in the
 * board header, and config may only pick from what it offers.
 *
 * A pin with no row is not assignable to anything.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_MODEL_H
#define PIN_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── What a pin may become ────────────────────────────────────────
 *
 * A row lists every function the HARDWARE can support on that pin, not what
 * is currently assigned, and the two are different things. A pyro firing pad
 * carries FN_PYRO_FIRE *and* the Lua functions it could take once its channel
 * is released -- that pairing is the whole feature. What may not happen is
 * holding both at once, which is an assignment-time rule rather than a
 * property of the hardware.
 *
 * FN_PYRO_FIRE on a pad means a firing FET gate is wired to it, so a Lua role
 * there is reaching real power hardware. */
#define FN_NONE 0u

/* Owned by the flight software when assigned. */
#define FN_PYRO_FIRE (1u << 0)   /* a per-channel firing element         */
#define FN_PYRO_COMMON (1u << 1) /* the element both channels need       */
#define FN_PYRO_SENSE (1u << 2)  /* continuity sense; ADC pads only      */
#define FN_BUZZER (1u << 3)
#define FN_UART_TX (1u << 4)
#define FN_UART_RX (1u << 5)
#define FN_I2C_SDA (1u << 6)
#define FN_I2C_SCL (1u << 7)
#define FN_LED (1u << 8)

/* Assignable to Lua. */
#define FN_DIGITAL (1u << 16) /* out / in, plain SIO                    */
#define FN_PWM (1u << 17)     /* dimmable out, software PWM on core1    */
#define FN_SERIAL (1u << 18)  /* PIO uart tx / rx                       */
#define FN_PIXEL (1u << 19)   /* WS2812 string                          */
#define FN_BRIDGE (1u << 20)  /* one half of a half-bridge pair         */
#define FN_ANALOG (1u << 21)  /* free-running sampled, read by a getter */

#define FN_BOARD_RESERVED                                                                                              \
    (FN_PYRO_FIRE | FN_PYRO_COMMON | FN_PYRO_SENSE | FN_BUZZER | FN_UART_TX | FN_UART_RX | FN_I2C_SDA | FN_I2C_SCL |   \
     FN_LED)
#define FN_LUA_ANY (FN_DIGITAL | FN_PWM | FN_SERIAL | FN_PIXEL | FN_BRIDGE | FN_ANALOG)

/* ── Power-group role ─────────────────────────────────────────────
 *
 * Deliberately not named "high" or "low": which side is switched per channel
 * and which is common differs by board, and a half-bridge needs one of each
 * rather than one of a particular polarity. BOARD_PYRO_TOPOLOGY says which
 * way round a given board is. */
typedef enum {
    PG_NONE = 0,
    PG_CH1,    /* the element only channel 1 needs */
    PG_CH2,    /* the element only channel 2 needs */
    PG_COMMON, /* the element both channels need   */
} pin_group_t;

#define PYRO_TOPO_HIGH_SWITCHED 1 /* per-channel high sides, common low */
#define PYRO_TOPO_LOW_SWITCHED 2  /* per-channel low sides, common high */

/* What protects the common path when both sides of a bridge conduct at once.
 * The web UI takes its shoot-through warning from this rather than a fixed
 * string, because the consequence is not the same on every board: MK1A loses
 * a one-shot fuse and the pyros with it, MK1B trips a PTC and recovers. */
#define PYRO_PROT_FUSE_ONESHOT 1
#define PYRO_PROT_PTC_LIMITED 2
#define PYRO_PROT_EFUSE 3

typedef struct {
    uint8_t pin;
    uint32_t functions;
    pin_group_t group;
} pin_cap_t;

/* RP2040 ADC pads. FN_PYRO_SENSE and FN_ANALOG are meaningful only here. */
#define PIN_IS_ADC_CAPABLE(p) ((p) >= 26 && (p) <= 29)

/* ── Build-time checks ────────────────────────────────────────────
 *
 * A board invokes PIN_CAPS_ASSERT(BOARD_PIN_CAPS) once. A wrong row fails the
 * build rather than producing a board that can be misconfigured -- same
 * argument as the PIO budget assertions in src/lua/lua_pio_platform.c. */

/* The sensor's I2C pads are never Lua-assignable under any configuration.
 * RP2040 fixes a pin's peripheral function by pin number, and on these boards
 * a Lua-reachable pad shares an I2C instance with the flight pressure sensor
 * -- GPIO18/19 are i2c1 on MK1C, whose MS5607 is on GPIO6/7. Barometric
 * pressure is a fixed function, so the pads carrying it never move. */
#define PIN_CAP_X_I2C_NEVER_LUA(pin, fn, pg)                                                                           \
    _Static_assert(!((fn) & (FN_I2C_SDA | FN_I2C_SCL)) || !((fn) & FN_LUA_ANY),                                        \
                   "pin " #pin " carries the sensor bus and must never be Lua-assignable");

/* A bridge half is one element of a power group, so a pad offering FN_BRIDGE
 * has to say which element it is. */
#define PIN_CAP_X_BRIDGE_HAS_GROUP(pin, fn, pg)                                                                        \
    _Static_assert(!((fn) & FN_BRIDGE) || (pg) != PG_NONE,                                                             \
                   "pin " #pin " offers FN_BRIDGE but declares no power-group role");

#define PIN_CAP_X_ANALOG_IS_ADC(pin, fn, pg)                                                                           \
    _Static_assert(!((fn) & (FN_PYRO_SENSE | FN_ANALOG)) || PIN_IS_ADC_CAPABLE(pin),                                   \
                   "pin " #pin " is declared analog but is not an RP2040 ADC pad");

#define PIN_CAP_X_GROUP_NEEDS_PYRO(pin, fn, pg)                                                                        \
    _Static_assert((pg) == PG_NONE || ((fn) & (FN_PYRO_FIRE | FN_PYRO_COMMON)),                                        \
                   "pin " #pin " has a power-group role but switches no pyro element");

#define PIN_CAP_X_COUNT_COMMON(pin, fn, pg) +((pg) == PG_COMMON ? 1 : 0)
#define PIN_CAP_X_COUNT_CH1(pin, fn, pg) +((pg) == PG_CH1 ? 1 : 0)
#define PIN_CAP_X_COUNT_CH2(pin, fn, pg) +((pg) == PG_CH2 ? 1 : 0)

#define PIN_CAPS_ASSERT(TABLE)                                                                                         \
    TABLE(PIN_CAP_X_I2C_NEVER_LUA)                                                                                     \
    TABLE(PIN_CAP_X_BRIDGE_HAS_GROUP)                                                                                  \
    TABLE(PIN_CAP_X_ANALOG_IS_ADC)                                                                                     \
    TABLE(PIN_CAP_X_GROUP_NEEDS_PYRO)                                                                                  \
    _Static_assert((0 TABLE(PIN_CAP_X_COUNT_COMMON)) <= 1, "more than one pin declares PG_COMMON");                     \
    _Static_assert((0 TABLE(PIN_CAP_X_COUNT_CH1)) <= 1, "more than one pin declares PG_CH1");                           \
    _Static_assert((0 TABLE(PIN_CAP_X_COUNT_CH2)) <= 1, "more than one pin declares PG_CH2");

/* ── Runtime lookup ───────────────────────────────────────────────── */

const pin_cap_t *pin_caps_table(int *count);

/* The row for a pin, or NULL when the board declares none. */
const pin_cap_t *pin_caps_find(uint8_t pin);

/* True when this board can offer a half-bridge. */
bool pin_caps_bridge_possible(void);

/* "high_switched" or "low_switched": which side of the pyro path each channel
 * switches. Lets the UI label a released pad correctly without the HTTP layer
 * including a board header. */
const char *pin_caps_topology_name(void);

/* What happens on THIS board if both sides of a bridge conduct at once, as a
 * sentence for the operator. Served to the web UI rather than written there,
 * because the consequence is not the same on every board and a UI holding its
 * own copy is a UI that will be wrong about one of them. */
const char *pin_caps_protection_note(void);

/* True when the pin is Lua's before any configuration releases anything:
 * Lua-capable and reserved for nothing. */
bool pin_caps_is_default_lua(const pin_cap_t *c);

/* Checks LUA_PIN_LIST against the table. Returns the first pin that is listed
 * but not actually free for Lua, or -1 when the list is consistent. Called at
 * boot: the list and the table are two statements of the same fact, and they
 * are written by hand in the same file. */
int pin_caps_check_lua_list(void);

#endif
