/*
 * Which paths a file call takes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VFS_PATH_H
#define VFS_PATH_H

#include <stdbool.h>
#include <stddef.h>

static inline bool vfs_path_ok(const char *path) {
    (void)path;
    return true;
}

#endif
