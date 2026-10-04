/*
 * Loading and storing the pin assignment: the file half of pin_assign.c,
 * whose rules stay free of I/O so the host tests can reach them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_STORE_H
#define PIN_STORE_H

#include "pad_claim.h"
#include "pin_assign.h"

#define PIN_STORE_PATH "pins.ini"
#define PIN_STORE_MAX 1024

/* Load pins.ini, validate it, and publish it as the live assignment.
 *
 * A file that fails validation is REJECTED WHOLE and the board falls back to
 * the board defaults -- nothing released, every pyro pad retained: half a
 * release leaves the flight software and a script each believing they own a
 * pin. With no pins.ini the defaults are also written, as a file to edit; a
 * pins.ini that cannot be read is left as it is. Call once at boot, before
 * lua_app_init().
 *
 * reason receives a short phrase for /api/status; "" when the load was clean. */
void pin_store_load(char *reason, int reason_len);

/* The live assignment. Never NULL. */
const pin_assign_t *pin_store_current(void);

/* True when a pad is actually driving the buzzer -- the board's own, or one
 * the operator assigned: "can this board beep". */
bool pin_store_has_buzzer(void);

/* Validate and write. Returns the verdict; nothing is written unless it is
 * PIN_OK. Never from the flight task. */
pin_verdict_t pin_store_save(const pin_assign_t *a);

/* Why the last load fell back, or "" when it did not. */
const char *pin_store_reason(void);

/* True when this pad is still the flight software's to drive. A board whose
 * continuity stimulus touches a per-channel pad asks, so it does not drive a
 * pad released to Lua. */
bool pin_store_owns(uint8_t pin);

/* Give every pad exactly one owner, from the live assignment: one write per
 * pad, so no pad can be both the flight software's and a script's. See
 * pad_claim.h. Once at boot, after pin_store_load() and before either table
 * is populated. */
void pin_store_claim_pads(void);

/* The pads one pyro channel switches, as a claim mask: its own element plus
 * the common. Empty when the board declares no such channel.
 *
 * From the board's capability table -- PG_CH1, PG_CH2, PG_COMMON -- so it is
 * the board that says which pad is which channel's, not this file. */
uint32_t pin_store_pyro_pads(uint8_t channel);

/* Fill cfg-shaped Lua pin roles from the live assignment, in the positional
 * order lua_plat_configure() expects. Returns how many entries were filled. */
int pin_store_lua_pins(lua_pin_cfg_t *out, int max);

/* The assigned bridge pair, if any. Returns true and fills the pins and the
 * name; false when nothing is assigned to a bridge.
 *
 * channel_pin is the per-channel element and common_pin the shared one, which
 * is the order pyro_bridge_program_init() wants -- SET drives the first and
 * side-set the second. */
bool pin_store_bridge(uint8_t *channel_pin, uint8_t *common_pin, const char **name);

#endif
