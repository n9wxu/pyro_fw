/*
 * The paths a file call takes [WEB-API-14]: '/'-separated names of letters,
 * digits, '.', '_' and '-', with an optional leading '/'. Nothing else
 * reaches a store: FatFs resolves "." and "..", so on a card
 * "/www/../config.ini" names config.ini, past the merge and the validation
 * /api/config applies [CFG-06, SYS-CFG-03]; '%', '\\' and an empty name have
 * no use here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VFS_PATH_H
#define VFS_PATH_H

#include <stdbool.h>
#include <stddef.h>

static inline bool vfs_name_char(char ch) {
    return ((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) || ((ch >= '0') && (ch <= '9')) || (ch == '.') ||
           (ch == '_') || (ch == '-');
}

static inline bool vfs_path_ok(const char *path) {
    if (path == NULL) {
        return false;
    }
    const char *p = (*path == '/') ? &path[1] : path;
    for (;;) {
        const char *name = p;
        while (vfs_name_char(*p)) {
            p++;
        }
        ptrdiff_t len = p - name;
        size_t n = (size_t)len;
        bool dots = ((n == 1u) && (name[0] == '.')) || ((n == 2u) && (name[0] == '.') && (name[1] == '.'));
        if ((n == 0u) || dots) {
            return false;
        }
        if (*p == '\0') {
            return true;
        }
        if (*p != '/') {
            return false;
        }
        p++;
    }
}

#endif
