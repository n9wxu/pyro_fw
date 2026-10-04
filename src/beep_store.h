/*
 * Loading and storing the beep table: the file half of beep_codes.c, as
 * pin_store.c is pin_assign.c's. In beep.ini, apart from config.ini, so the
 * beep map and the flight settings version independently.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BEEP_STORE_H
#define BEEP_STORE_H

#include "beep_codes.h"

#define BEEP_STORE_PATH "beep.ini"
/* BEEP_PERSONALITY_COUNT personalities of BEEP_REASON_COUNT + 4 keys each,
 * plus the header; test_beep_codes.c serialises the longest into it. */
#define BEEP_STORE_MAX 1024

/* Load beep.ini, validate it, and publish it as the live table.
 *
 * A file that fails validation is REJECTED WHOLE and the board falls back to
 * the shipped codes: half a beep map means an operator hears a code that
 * means one thing on the board and another in the UI.
 *
 * With no beep.ini at all the shipped codes are written out, so there is a
 * file to edit; one that cannot be read is left as it is. Call once at boot.
 *
 * reason receives a short phrase for /api/status; "" when the load was
 * clean. */
void beep_store_load(char *reason, int reason_len);

/* The live table. Never NULL. */
const beep_table_t *beep_store_current(void);

/* Validate and write. Nothing is written unless the verdict is BEEP_OK.
 * Never from the flight task. */
beep_verdict_t beep_store_save(const beep_table_t *t);

/* Why the last load fell back, or "" when it did not. */
const char *beep_store_reason(void);

/* How an outcome sounds under the live, active personality. This is what the
 * firmware calls; nothing outside this module should reach for a raw spec. */
beep_spec_t beep_for(beep_reason_t r);

/* Play an outcome, with the active personality's cadence. */
void beep_say(beep_reason_t r);

#endif
