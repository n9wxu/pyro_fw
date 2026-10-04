/*
 * The paths a file call takes [WEB-API-14]: '/'-separated names of letters,
 * digits, '.', '_' and '-', with an optional leading '/'. Nothing else
 * reaches a store: littlefs and FatFs resolve "." and "..", so
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
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
           ch == '-';
}

static inline bool vfs_path_ok(const char *path) {
    if (!path) {
        return false;
    }
    const char *p = (*path == '/') ? path + 1 : path;
    for (;;) {
        const char *name = p;
        while (vfs_name_char(*p)) {
            p++;
        }
        size_t n = (size_t)(p - name);
        bool dots = (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.');
        if (n == 0 || dots) {
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
