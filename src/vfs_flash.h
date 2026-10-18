/*
 * The board's own files: alfs over the filesystem area [DD-100].
 *
 * Each file is a chain of flash pages found by its name, which is at most
 * ALFS_NAME_MAX bytes. A write replaces a file whole: the new copy is written
 * beside the old one and takes its place when it closes, so a write that
 * dies part-way leaves the old file as it was. Nothing here erases but
 * vfs_flash_clean(): a write takes only erased pages, and the pages a
 * replaced or removed file leaves wait for the cleanup.
 *
 * Every call takes the store's lock (fs_lock.h), held for the one call.
 * Names arrive bare, without a leading '/'.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VFS_FLASH_H
#define VFS_FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "alfs_fs.h"

#define VFS_FLASH_NOENT (-2)
#define VFS_FLASH_ERR (-1)

/* A file's block: alfs keeps a writer's bytes in it until a page fills. */
#define VFS_FLASH_BUF_SIZE 1024

enum { VFS_FLASH_READ = 1, VFS_FLASH_WRITE, VFS_FLASH_APPEND };

typedef struct {
    uint8_t mode; /* 0 when closed */
    char name[ALFS_NAME_MAX + 1];
    void *buf;
    uint32_t written;
    union {
        alfs_file_t w;
        alfs_reader_t r;
    } u;
} vfs_flash_file_t;

/* Reads every page's header once and keeps a table of the files. Read-only
 * until a mount with writable true, which the board makes only once it knows
 * the chip holds the whole area. */
int vfs_flash_mount(const alfs_area_t *area, const alfs_driver_t *drv, bool writable);
bool vfs_flash_mounted(void);

/* VFS_FLASH_WRITE: a new copy, which replaces the old when it closes.
 * VFS_FLASH_APPEND: more of a file a reset left open, or a new one; a closed
 * file is final. buf: VFS_FLASH_BUF_SIZE bytes, the file's until closed. */
int vfs_flash_open(vfs_flash_file_t *f, const char *name, int mode, void *buf);
int vfs_flash_read(vfs_flash_file_t *f, void *buf, uint32_t n);
int vfs_flash_write(vfs_flash_file_t *f, const void *buf, uint32_t n);
int vfs_flash_sync(vfs_flash_file_t *f); /* every byte written so far reaches the flash */
int vfs_flash_close(vfs_flash_file_t *f);
int32_t vfs_flash_size(vfs_flash_file_t *f);
int vfs_flash_rewind(vfs_flash_file_t *f);

int vfs_flash_remove(const char *name);
/* A copy under the new name, then the old name removed. */
int vfs_flash_rename(const char *from, const char *to);
int32_t vfs_flash_stat_size(const char *name);

/* From the files' own sizes, a read a file, never the whole area: free is
 * every page no file holds, the pages the cleanup will erase among them. */
int vfs_flash_space(uint64_t *free_bytes, uint64_t *total_bytes);

/* One sector erased, if one holds nothing live: 1 when there may be more, 0
 * when the area is clean, <0 on a fault. Never waits for the lock, and waits
 * while a file is open for reading: 1 then too. */
int vfs_flash_clean(void);

#endif
