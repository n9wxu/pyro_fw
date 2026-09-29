/*
 * The files, wherever they live [DD-076].
 *
 * One interface over two stores: littlefs on the internal flash, always
 * mounted (lfs_mount.h), and FatFs on an SD card, on a board built with one
 * and while a card is mounted. A path goes to the card when there is one,
 * except the few that belong to the board rather than the card -- its
 * identity and the recovery marker -- which stay internal.
 *
 * The configuration files are the card's, and each is copied into littlefs
 * whenever the two differ (vfs_mirror()), so a board whose card is missing
 * or unreadable still boots with the configuration it last had. A file the
 * card lacks is read from littlefs, so a blank card still serves the web
 * pages; every write goes to the card.
 *
 * Every call may block on the store's lock: never from the flight task, which
 * reads the one file it needs through hal_fs_read_cached().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stdint.h>
#include <lfs.h>
#if PYRO_HAS_SD
#include "ff.h"
#endif

enum { VFS_NONE = 0, VFS_LFS, VFS_FAT };

#define VFS_RD 0x01
#define VFS_WR 0x02     /* create, truncate */
#define VFS_APPEND 0x04 /* with VFS_WR: create, keep, write at the end */

/* Errors: VFS_NOENT for no such file, VFS_ERR for anything else. */
#define VFS_NOENT (-2)
#define VFS_ERR (-1)

typedef struct {
    uint8_t kind;
    union {
        lfs_file_t lfs;
#if PYRO_HAS_SD
        FIL fat;
#endif
    } u;
    struct lfs_file_config lcfg;
} vfs_file_t;

/* lfs_buf: littlefs's cache for this file, LFS_FILE_BUF_SIZE bytes, used only
 * when the file is internal; a card file needs none. */
int vfs_open(vfs_file_t *f, const char *path, int flags, void *lfs_buf);
int vfs_read(vfs_file_t *f, void *buf, uint32_t n);        /* bytes, or <0 */
int vfs_write(vfs_file_t *f, const void *buf, uint32_t n); /* bytes, or <0 */
int vfs_sync(vfs_file_t *f);
int vfs_close(vfs_file_t *f);
int32_t vfs_size(vfs_file_t *f);
int vfs_rewind(vfs_file_t *f);

int vfs_remove(const char *path); /* VFS_NOENT when there was none */
int vfs_rename(const char *from, const char *to);
int vfs_mkdir(const char *path);         /* 0 also when it exists */
int32_t vfs_stat_size(const char *path); /* size, or VFS_NOENT / VFS_ERR */

/* The store a path goes to now: VFS_LFS or VFS_FAT. */
int vfs_route(const char *path);

/* Bytes free and in all, on the store a path goes to. */
int vfs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

/* True while an SD card is mounted and taking files. */
bool vfs_sd_mounted(void);

/* At boot, the configuration files: each the card holds and littlefs does
 * not, or holds differently, is copied into littlefs; each littlefs holds and
 * the card does not is copied onto the card, so a blank card in a configured
 * board takes its configuration rather than defaults. Returns how many were
 * copied. */
int vfs_mirror(void);

/* The same for one path, after a write to it. 1 when it copied. */
int vfs_mirror_one(const char *path);

/* A write to a configuration file lands on the card and in littlefs both. */
bool vfs_is_config(const char *path);

#endif
