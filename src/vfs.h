/*
 * The files, wherever they live [DD-076].
 *
 * One interface over two stores: the board's own files on the internal flash
 * (vfs_flash.h), always mounted, and FatFs on an SD card, on a board built
 * with one and while a card is mounted. A path goes to the card when there is
 * one, except the few that belong to the board rather than the card -- its
 * identity and the recovery marker -- which stay internal.
 *
 * The configuration files are the card's, and each is copied into the flash
 * whenever the two differ (vfs_mirror()), so a board whose card is missing
 * or unreadable still boots with the configuration it last had. A file the
 * card lacks is read from the flash, so a blank card still serves the web
 * pages; every write goes to the card.
 *
 * On the flash a write replaces a file whole: the old file stays until the
 * new one closes. Nothing erases there but vfs_clean().
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
#include "vfs_flash.h"
#include "vfs_path.h"
#if PYRO_HAS_SD
#include "ff.h"
#endif

enum { VFS_NONE = 0, VFS_FLASH, VFS_FAT };

#define VFS_RD 0x01
#define VFS_WR 0x02     /* create, or replace whole */
#define VFS_APPEND 0x04 /* with VFS_WR: create, keep, write at the end */

/* Errors: VFS_NOENT for no such file, VFS_ERR for anything else, a path
 * vfs_path_ok() refuses included. */
#define VFS_NOSPC VFS_FLASH_NOSPC
#define VFS_NOENT VFS_FLASH_NOENT
#define VFS_ERR VFS_FLASH_ERR

/* A file's buffer on the flash; a card file needs none. */
#define VFS_BUF_SIZE VFS_FLASH_BUF_SIZE

typedef struct {
    uint8_t kind;
    union {
        vfs_flash_file_t flash;
#if PYRO_HAS_SD
        FIL fat;
#endif
    } u;
} vfs_file_t;

/* Reads the flash's files once; writable once the board knows the chip holds
 * the whole area. Before the scheduler starts. */
int vfs_mount(bool writable);

/* buf: VFS_BUF_SIZE bytes, the file's until closed, used only when the file
 * is on the flash. */
int vfs_open(vfs_file_t *f, const char *path, int flags, void *buf);
int vfs_read(vfs_file_t *f, void *buf, uint32_t n);        /* bytes, or <0 */
int vfs_write(vfs_file_t *f, const void *buf, uint32_t n); /* bytes, or <0 */
int vfs_sync(vfs_file_t *f); /* every byte written so far stored */
int vfs_close(vfs_file_t *f);
int32_t vfs_size(vfs_file_t *f);
int vfs_rewind(vfs_file_t *f);

int vfs_remove(const char *path); /* VFS_NOENT when there was none */
int vfs_rename(const char *from, const char *to); /* on the flash, a copy */
int vfs_mkdir(const char *path);         /* 0 also when it exists */
int32_t vfs_stat_size(const char *path); /* size, or VFS_NOENT / VFS_ERR */

/* The store a path goes to now: VFS_FLASH or VFS_FAT. */
int vfs_route(const char *path);

/* Bytes free and in all, on the store a path goes to. */
int vfs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

/* The room bytes written and then synced take there, as vfs_space() counts. */
uint32_t vfs_synced_cost(const char *path, uint32_t bytes);

/* True while an SD card is mounted and taking files. */
bool vfs_sd_mounted(void);

/* One sector of the flash erased, if one holds nothing a file needs: 1 when
 * there may be more, 0 when every unused page is erased, <0 on a fault. */
int vfs_clean(void);

/* At boot, the configuration files: each the card holds and the flash does
 * not, or holds differently, is copied into the flash; each the flash holds
 * and the card does not is copied onto the card, so a blank card in a
 * configured board takes its configuration rather than defaults. Returns how
 * many were copied. */
int vfs_mirror(void);

/* The same for one path, after a write to it. 1 when it copied. */
int vfs_mirror_one(const char *path);

/* A write to a configuration file lands on the card and in the flash both. */
bool vfs_is_config(const char *path);

#endif
