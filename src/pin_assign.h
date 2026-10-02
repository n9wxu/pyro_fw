/*
 * Which function each pin is actually assigned, and whether that is legal.
 *
 * pin_caps.h says what a pin MAY become, and the board owns it; this says what
 * it IS, and the operator owns it. Stored in pins.ini, apart from config.ini,
 * so the hardware map and the flight settings version independently.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_ASSIGN_H
#define PIN_ASSIGN_H

#include "ground_test_switch.h"
#include "lua_platform_cfg.h"
#include "pin_model.h"
#include <stdbool.h>
#include <stdint.h>

#define PIN_ASSIGN_MAX_GPIO 30 /* RP2040 has GPIO0..29 */

/* ── The release model ────────────────────────────────────────────
 *
 * A pyro channel releases independently; the common releases only once BOTH
 * channels are released, because until then it is still half of the retained
 * channel's firing path. With one channel released, a fire on the other
 * asserts the common for its pulse, and the released side has a return path
 * meanwhile; the UI warns. */

/* "leave the buzzer where the board put it". Not 0, which is a real GPIO. */
#define PIN_BUZZER_BOARD 255u

/* [GND-TEST-12] A ground test pad not assigned. The wirings, GT_WIRING_*,
 * are ground_test_switch.h's. */
#define PIN_GT_UNSET 255u

typedef struct {
    bool pyro1_released;
    bool pyro2_released;

    /* [PIN-BUZZ-01, PIN-BUZZ-02] The pad driving the buzzer, or
     * PIN_BUZZER_BOARD for the board's own. Any digital pad can (MK1A fits no
     * buzzer, so this is how it gets one); a flight-software assignment,
     * reserved against Lua like the board's own pad. */
    uint8_t buzzer_pin;

    /* The ground test switch: a flight-software assignment, reserved against
     * Lua as the buzzer's pad is. gt_pin is read; gt_drive_pin is driven, for
     * a switch across two pads. PIN_GT_UNSET where not used. */
    uint8_t gt_wiring;
    uint8_t gt_pin;
    uint8_t gt_drive_pin;

    /* Indexed by GPIO. LUA_ROLE_OFF where nothing is assigned. */
    uint8_t role[PIN_ASSIGN_MAX_GPIO];
    char name[PIN_ASSIGN_MAX_GPIO][LUA_NAME_MAX];
} pin_assign_t;

/* Why an assignment was refused. Ordered so a caller can report the first
 * problem it hits and an operator can act on it. */
typedef enum {
    PIN_OK = 0,
    PIN_ERR_UNKNOWN_PIN,        /* the board declares no row for it        */
    PIN_ERR_NOT_CAPABLE,        /* the pin cannot take that role           */
    PIN_ERR_PYRO_RETAINED,      /* the channel still owns the pin          */
    PIN_ERR_COMMON_HELD,        /* the other channel still needs the common */
    PIN_ERR_BRIDGE_UNSUPPORTED, /* this board offers no bridge         */
    PIN_ERR_BRIDGE_INCOMPLETE,  /* a bridge needs a channel and the common */
    PIN_ERR_DUPLICATE_NAME,     /* two resources share one Lua name        */
    PIN_ERR_BUZZER_NOT_CAPABLE, /* the pad cannot drive a buzzer       */
    PIN_ERR_BUZZER_BUSY,        /* the pad is already doing something  */
    PIN_ERR_GT_NOT_CAPABLE,     /* the pad cannot read a switch        */
    PIN_ERR_GT_BUSY,            /* the pad is already doing something  */
    PIN_ERR_GT_INCOMPLETE,      /* the wiring lacks a pad it needs     */
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
 * and the common while either channel is retained both answer true, as does
 * whichever pad is currently driving the buzzer. */
bool pin_assign_is_reserved(const pin_assign_t *a, uint8_t pin);

/* The pad the buzzer is actually on: the assignment if there is one, else the
 * board's own FN_BUZZER pad, else PIN_BUZZER_BOARD when the board fits none
 * and nothing has been assigned -- i.e. this board currently has no buzzer. */
uint8_t pin_assign_buzzer_pin(const pin_assign_t *a);

/* Parse pins.ini over an existing assignment. Unknown keys are ignored, for
 * the same forward-compatibility reason config.ini ignores them (CFG-08).
 * Mutates buf. */
void pin_assign_parse_ini(char *buf, pin_assign_t *a);

/* Serialise. Returns bytes written, or -1 when it would not fit. */
int pin_assign_serialize_ini(const pin_assign_t *a, char *buf, int max_len);

/* A human phrase for a verdict, for the HTTP response and the UI. */
const char *pin_assign_strerror(pin_err_t e);

/* ── The role vocabulary ──────────────────────────────────────────
 *
 * Served to the web UI rather than copied into it, so a menu offers only what
 * pin_assign_validate() accepts. Index 0 is always "off". */
int pin_assign_role_count(void);
const char *pin_assign_role_name(int idx);

/* The capability bit a pin must carry to take this role; 0 for "off", which
 * every pin can take. */
uint32_t pin_assign_role_needs(int idx);

/* The name of a role VALUE (a LUA_ROLE_*), as stored in pin_assign_t.role[],
 * so the enum's numbering need not match the vocabulary's index. */
const char *pin_assign_role_name_of(uint8_t role);

#endif
