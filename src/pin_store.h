/*
 * Loading and storing the pin assignment.
 *
 * Split from pin_assign.c so the rules stay free of file I/O and can be
 * tested on the host. This half knows about littlefs and about the legacy
 * config keys; that half knows about the rules.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PIN_STORE_H
#define PIN_STORE_H

#include "config.h"
#include "pin_assign.h"

#define PIN_STORE_PATH "pins.ini"
#define PIN_STORE_MAX 1024

/* Load pins.ini, validate it, and publish it as the live assignment.
 *
 * A file that fails validation is REJECTED WHOLE and the board falls back to
 * the migrated legacy assignment: a partially applied pin map is the one
 * outcome worse than none, because half a release leaves the flight software
 * and a script each believing they own a pin.
 *
 * With no pins.ini at all, the assignment is migrated from cfg's lua_p*
 * keys, so a board that has never seen this feature keeps the Lua pins it
 * already had. Call once at boot, before lua_app_init().
 *
 * reason receives a short phrase for /api/status; it is set to "" when the
 * load was clean. */
void pin_store_load(const config_t *cfg, char *reason, int reason_len);

/* The live assignment. Never NULL. */
const pin_assign_t *pin_store_current(void);

/* Validate and write. Returns the verdict; nothing is written unless it is
 * PIN_OK. Must be called inside the flash window. */
pin_verdict_t pin_store_save(const pin_assign_t *a);

/* Why the last load fell back, or "" when it did not. */
const char *pin_store_reason(void);

/* True when this pad is still the flight software's to drive.
 *
 * For a board whose continuity stimulus touches a per-channel pad: the pad
 * belongs to Lua once its channel is released, and core0 writing it would
 * fight core1 for the same SIO register. Asking is cheaper than the
 * alternative, which is the flight loop stamping a released output low ten
 * times a second. */
bool pin_store_owns(uint8_t pin);

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
