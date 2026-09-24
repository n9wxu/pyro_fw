/*
 * Loading and storing the beep table.
 *
 * Split from beep_codes.c so the rules stay free of file I/O and can be
 * tested on the host -- the same split pin_assign.c and pin_store.c use.
 *
 * In its own file rather than config.ini: thirteen codes do not fit the
 * 512-byte budget that file is read back with, and the hardware map and the
 * flight settings should version independently.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BEEP_STORE_H
#define BEEP_STORE_H

#include "beep_codes.h"

#define BEEP_STORE_PATH "beep.ini"
#define BEEP_STORE_MAX 512

/* Load beep.ini, validate it, and publish it as the live table.
 *
 * A file that fails validation is REJECTED WHOLE and the board falls back to
 * the shipped codes: half a beep map means an operator hears a code that
 * means one thing on the board and another in the UI.
 *
 * With no beep.ini at all the shipped codes are written out, so there is a
 * file to edit. Call once at boot.
 *
 * reason receives a short phrase for /api/status; "" when the load was
 * clean. */
void beep_store_load(char *reason, int reason_len);

/* The live table. Never NULL. */
const beep_table_t *beep_store_current(void);

/* Validate and write. Nothing is written unless the verdict is BEEP_OK.
 * Must be called inside the flash window. */
beep_verdict_t beep_store_save(const beep_table_t *t);

/* Why the last load fell back, or "" when it did not. */
const char *beep_store_reason(void);

/* The code for a reason, from the live table. This is what the firmware
 * calls; nothing outside this module should reach for a raw code. */
uint8_t beep_for(beep_reason_t r);

#endif
