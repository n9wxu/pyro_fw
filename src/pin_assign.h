/*
 * Which function each pin is actually assigned, and whether that is legal.
 *
 * pin_caps.h says what a pin MAY become; this says what it IS. The split
 * matters because the board owns the first and an operator owns the second,
 * and validation is the only thing standing between them.
 *
 * Stored in pins.ini rather than config.ini. config.ini serialises to ~451 of
 * the 512 bytes hal_config_load() can read back, and a per-pin table does not
 * fit in 61 bytes. Keeping them apart also means the hardware map and the
 * flight settings version independently.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_ASSIGN_H
#define PIN_ASSIGN_H

#include "lua_platform_cfg.h"
#include "pin_model.h"
#include <stdbool.h>
#include <stdint.h>

#define PIN_ASSIGN_MAX_GPIO 30 /* RP2040 has GPIO0..29 */

/* ── The release model ────────────────────────────────────────────
 *
 * A pyro channel releases independently; the common releases only once BOTH
 * channels are released, because until then it is still half of the retained
 * channel's firing path.
 *
 * On a board with two switched high sides and one common low side (MK1A,
 * MK1B) that yields:
 *
 *   one channel released   1 digital pin, that channel's own side
 *   both released          3 digital pins, or 1 half-bridge + 1 digital
 *
 * Releasing one channel while the other still fires has a consequence the
 * firmware cannot prevent: pyro_fire() on the retained channel asserts the
 * common for 500 ms, and for that window the released side has a return path.
 * The UI warns; the released side genuinely is just a digital pin. */
typedef struct {
    bool pyro1_released;
    bool pyro2_released;

    /* Indexed by GPIO. LUA_ROLE_OFF where nothing is assigned. */
    uint8_t role[PIN_ASSIGN_MAX_GPIO];
    char name[PIN_ASSIGN_MAX_GPIO][LUA_NAME_MAX];
} pin_assign_t;

/* Why an assignment was refused. Ordered so a caller can report the first
 * problem it hits and an operator can act on it. */
typedef enum {
    PIN_OK = 0,
    PIN_ERR_UNKNOWN_PIN,    /* the board declares no row for it        */
    PIN_ERR_NOT_CAPABLE,    /* the pin cannot take that role           */
    PIN_ERR_PYRO_RETAINED,  /* the channel still owns the pin          */
    PIN_ERR_COMMON_HELD,    /* the other channel still needs the common */
    PIN_ERR_BRIDGE_UNSUPPORTED, /* this board offers no bridge         */
    PIN_ERR_BRIDGE_INCOMPLETE,  /* a bridge needs a channel and the common */
    PIN_ERR_DUPLICATE_NAME, /* two resources share one Lua name        */
} pin_err_t;

typedef struct {
    pin_err_t err;
    uint8_t pin;      /* the pin the problem is about, when it has one */
    const char *what; /* a short phrase for the operator               */
} pin_verdict_t;

/* Start from the board's defaults: nothing released, and every pin the board
 * grants Lua by default left at LUA_ROLE_OFF for configuration to fill in. */
void pin_assign_defaults(pin_assign_t *a);

/* Check the whole assignment against the board's table and the release
 * rules. Returns the first problem, or PIN_OK.
 *
 * Whole-file: a partially applied pin map is the one outcome worse than
 * none, so a caller that gets anything but PIN_OK must keep the previous
 * assignment rather than take the good rows. */
pin_verdict_t pin_assign_validate(const pin_assign_t *a);

/* True when this pin is currently the flight software's. A retained pyro pin
 * and the common while either channel is retained both answer true. */
bool pin_assign_is_reserved(const pin_assign_t *a, uint8_t pin);

/* Parse pins.ini over an existing assignment. Unknown keys are ignored, for
 * the same forward-compatibility reason config.ini ignores them (CFG-08).
 * Mutates buf. */
void pin_assign_parse_ini(char *buf, pin_assign_t *a);

/* Serialise. Returns bytes written, or -1 when it would not fit. */
int pin_assign_serialize_ini(const pin_assign_t *a, char *buf, int max_len);

/* A human phrase for a verdict, for the HTTP response and the UI. */
const char *pin_assign_strerror(pin_err_t e);

#endif
