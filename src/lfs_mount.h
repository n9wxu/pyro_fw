/*
 * The one littlefs mount [DD-073].
 *
 * Mounted once at boot and never unmounted. littlefs is built with
 * LFS_THREADSAFE: every public call takes the mount's lock (littlefs_driver.c),
 * so any task but the flight task may use it, and each open file brings its
 * own cache buffer. Every program and erase runs under flash_op()'s lockout.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LFS_MOUNT_H
#define LFS_MOUNT_H

#include <lfs.h>
#include <stdbool.h>

extern lfs_t g_lfs;
extern const struct lfs_config lfs_pico_flash_config;

/* True once hal_fs_mount() has mounted g_lfs. */
bool lfs_mounted(void);

/* Makes the mount's lock. Before the scheduler starts (hal_fs_mount()); until
 * then littlefs runs unlocked, on the one thread there is. */
void lfs_lock_init(void);

/* An open file's cache, and littlefs's read and program caches: four pages.
 * Not part of the on-flash format. Small, so one program operation stops the
 * system for four pages at most. */
#define LFS_FILE_BUF_SIZE 1024

#endif
