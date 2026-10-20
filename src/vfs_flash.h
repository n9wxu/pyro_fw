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
 * A writer whose close fails stays open in alfs, which only a mount lets go.
 * Once no other file is open the store is mounted again, as a restart
 * mounts it, and the file is as a power cut would have left it; a remove of
 * it asked for meanwhile is done then [WEB-API-18].
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

#define VFS_FLASH_BUSY (-4)  /* the file is open */
#define VFS_FLASH_NOSPC (-3) /* no erased page left: a cleanup makes room */
#define VFS_FLASH_NOENT (-2)
#define VFS_FLASH_ERR (-1)

/* A file's block: alfs keeps a writer's bytes in it until a page fills. */
#define VFS_FLASH_BUF_SIZE 1024

/* The files the store keeps track of: a board's configuration, web pages,
 * script, identity and flight log are about a dozen, and a power cut can
 * leave a second copy of one. */
#define VFS_FLASH_FILES_MAX 48

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

/* VFS_FLASH_BUSY while the file is open, but for a writer whose close
 * failed: its remove is done when it is let go. */
int vfs_flash_remove(const char *name);
/* A copy under the new name, then the old name removed. */
int vfs_flash_rename(const char *from, const char *to);
int32_t vfs_flash_stat_size(const char *name);

/* From the files' own sizes, a read a file, never the whole area: free is
 * every page no file holds, the pages the cleanup will erase among them. */
int vfs_flash_space(uint64_t *free_bytes, uint64_t *total_bytes);

/* The room bytes take when a sync follows them: whole pages, since a sync
 * stores its partial page. */
uint32_t vfs_flash_synced_cost(uint32_t bytes);

/* One sector erased, if one holds nothing live: 1 when there may be more, 0
 * when the area is clean, <0 on a fault. Never waits for the lock, and waits
 * while a file is open for reading: 1 then too. */
int vfs_flash_clean(void);

/* [USB-10] The wait between the pad's cleanup steps: none while a USB host
 * is attached and test mode is off, where no launch can be declared; a
 * second otherwise, so an erase stalls the pad no more than once a second. */
#define VFS_FLASH_CLEAN_PERIOD_MS 1000u
static inline uint32_t vfs_flash_clean_wait_ms(bool grounded_on_usb) {
    return grounded_on_usb ? 0u : VFS_FLASH_CLEAN_PERIOD_MS;
}

/* [WEB-API-19] The store as its pages are now, in bytes of file data: what
 * the files hold, what is erased, and what no file needs but is not yet
 * erased -- some of it in sectors files still share, which the cleanup
 * erases once those files go. cleaning: it has sectors left to erase.
 * Reads every page's header. */
typedef struct {
    uint32_t bytes, in_files, erased, unused, bad_sectors;
    bool cleaning;
} vfs_flash_survey_t;
int vfs_flash_survey(vfs_flash_survey_t *out);

/* Each file, under the store's lock: its name, its size once closed (0
 * before), and whether it is closed. The count, or <0. each must not call
 * into the store. */
typedef void (*vfs_flash_each_t)(void *ctx, const char *name, uint32_t size, bool closed);
int vfs_flash_list(vfs_flash_each_t each, void *ctx);

#endif
