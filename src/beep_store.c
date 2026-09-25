/*
 * Loading and storing the beep table. See beep_store.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "beep_store.h"
#include "hal.h"
#include "buzzer.h"
#include <stdio.h>
#include <string.h>

static beep_table_t live;
static char load_reason[96];

const beep_table_t *beep_store_current(void) {
    return &live;
}

const char *beep_store_reason(void) {
    return load_reason;
}

beep_spec_t beep_for(beep_reason_t r) {
    return beep_codes_spec(&live, r);
}

void beep_say(beep_reason_t r) {
    const beep_personality_t *p = beep_codes_active(&live);
    beep_spec_t sp = beep_codes_spec(&live, r);
    buzzer_play_spec(&sp, p->gap_ms, p->repeat);
}

void beep_store_load(char *reason, int reason_len) {
    load_reason[0] = '\0';

    beep_table_t defaults;
    beep_codes_defaults(&defaults);

    char buf[BEEP_STORE_MAX];
    int n = hal_fs_read_file(BEEP_STORE_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        /* Write the shipped codes out, so the operator has a file to edit
         * rather than an absence to guess at -- the same reason pin_store
         * writes its defaults. At boot the flash window is still open. */
        live = defaults;
        char out[BEEP_STORE_MAX];
        int w = beep_codes_serialize_ini(&defaults, out, (int)sizeof(out));
        bool wrote = (w > 0) && (hal_fs_write_file(BEEP_STORE_PATH, out, w) == 0);
        snprintf(load_reason, sizeof(load_reason), "no %s; shipped codes%s", BEEP_STORE_PATH,
                 wrote ? " written" : " (could not write)");
        return;
    }

    buf[n] = '\0';
    beep_table_t from_file = defaults;
    beep_codes_parse_ini(buf, &from_file);

    beep_verdict_t v = beep_codes_validate(&from_file);
    if (v.err == BEEP_OK) {
        live = from_file;
    } else {
        live = defaults;
        snprintf(load_reason, sizeof(load_reason), "beep.ini rejected: %s (%s)", beep_codes_strerror(v.err),
                 v.reason >= 0 ? beep_codes_key((beep_reason_t)v.reason) : "table");
    }

    if (reason && reason_len > 0) {
        snprintf(reason, (size_t)reason_len, "%s", load_reason);
    }
}

beep_verdict_t beep_store_save(const beep_table_t *t) {
    beep_verdict_t v = beep_codes_validate(t);
    if (v.err != BEEP_OK) {
        return v;
    }

    char buf[BEEP_STORE_MAX];
    int n = beep_codes_serialize_ini(t, buf, (int)sizeof(buf));
    if (n <= 0) {
        beep_verdict_t bad = {BEEP_ERR_DIGIT_RANGE, -1, -1, "table does not fit beep.ini"};
        return bad;
    }
    if (hal_fs_write_file(BEEP_STORE_PATH, buf, n) != 0) {
        beep_verdict_t bad = {BEEP_ERR_DIGIT_RANGE, -1, -1, "could not write beep.ini"};
        return bad;
    }

    live = *t;
    return v;
}
