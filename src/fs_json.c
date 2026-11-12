/*
 * /api/fs. See fs_json.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fs_json.h"

#include "small_fmt.h"

#include <stdarg.h>

static void put_text(fs_json_t *j, const char *fmt, ...) {
    if (j->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int room = j->cap - j->len;
    int n = small_vsnprintf(&j->buf[j->len], (size_t)room, fmt, ap);
    va_end(ap);
    if ((n < 0) || (n >= room)) {
        j->overflow = true;
    } else {
        j->len += n;
    }
}

static bool path_char(char c) {
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '.') ||
           (c == '_') || (c == '-') || (c == '/');
}

void fs_json_begin(fs_json_t *j, char *buf, int cap, const fs_space_t *s) {
    *j = (fs_json_t){buf, cap, 0, true, cap <= 0};
    put_text(j,
             "{\"store\":\"flash\",\"bytes\":%lu,\"in_files\":%lu,\"free\":%lu,\"cleanup\":\"%s\","
             "\"cleaned_sectors\":%lu,\"sectors\":%lu,\"sector_pages\":%lu,\"page_bytes\":%lu,\"files\":[",
             (unsigned long)s->bytes, (unsigned long)s->in_files, (unsigned long)s->free, s->cleanup,
             (unsigned long)s->cleaned, (unsigned long)s->sectors, (unsigned long)s->sector_pages,
             (unsigned long)s->page_bytes);
}

void fs_json_file(fs_json_t *j, const char *name, uint32_t size, bool closed, bool board) {
    put_text(j, "%s{\"path\":\"/", j->first ? "" : ",");
    j->first = false;
    for (const char *p = name; *p != '\0'; p++) {
        put_text(j, "%c", path_char(*p) ? *p : '?');
    }
    if (closed) {
        put_text(j, "\",\"bytes\":%lu,\"closed\":true", (unsigned long)size);
    } else {
        put_text(j, "\",\"bytes\":null,\"closed\":false");
    }
    put_text(j, ",\"board\":%s}", board ? "true" : "false");
}

int fs_json_end(fs_json_t *j) {
    put_text(j, "]}");
    return j->overflow ? -1 : j->len;
}

int fs_json_scan(char *buf, int cap, uint32_t sector, const uint8_t *erased, int n) {
    fs_json_t j = {buf, cap, 0, true, cap <= 0};
    put_text(&j, "{\"sector\":%lu,\"erased\":[", (unsigned long)sector);
    for (int i = 0; i < n; i++) {
        put_text(&j, "%s%u", (i != 0) ? "," : "", (unsigned)erased[i]);
    }
    return fs_json_end(&j);
}
