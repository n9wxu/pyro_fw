/*
 * See boot_record.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "boot_record.h"
#include "flight_resume.h"
#include <stdio.h>
#include <string.h>

void boot_record_rows(const boot_facts_t *f, boot_rows_t rows) {
    if (f->stage >= 0)
        snprintf(rows[0], sizeof(rows[0]), "BOOT cause=%s wd=%d stage=%ld at=%lu ms", f->cause, f->watchdog ? 1 : 0,
                 (long)f->stage, (unsigned long)f->stage_ms);
    else
        snprintf(rows[0], sizeof(rows[0]), "BOOT cause=%s wd=%d stage=none", f->cause, f->watchdog ? 1 : 0);
    snprintf(rows[1], sizeof(rows[1]), "BOOT fw=%s flash=%s %lukB usb=%d", f->fw, f->flash, (unsigned long)f->flash_kb,
             f->usb ? 1 : 0);
    snprintf(rows[2], sizeof(rows[2]), "BOOT start=%s last=%s", f->start, f->last_log[0] ? f->last_log : "none");
}

const char *boot_record_cause(int reset_cause) {
    switch (reset_cause) {
    case RESET_POWER_EVENT:
        return "power";
    case RESET_RUN_PIN:
        return "run_pin";
    case RESET_SOFTWARE:
        return "software";
    case RESET_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}

int boot_record_lines(boot_rows_t rows, char *out, int cap) {
    int len = 0;
    for (int r = 0; r < BOOT_RECORD_ROWS; r++) {
        int n = (int)strlen(rows[r]);
        if (len + n + 1 > cap)
            return 0;
        memcpy(out + len, rows[r], (size_t)n);
        len += n;
        out[len++] = '\n';
    }
    return len;
}
