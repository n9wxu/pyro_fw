/*
 * The board's own files. See vfs_flash.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vfs_flash.h"
#include "fs_lock.h"
#include <string.h>

/* A board's files are its configuration, web pages, script, identity and
 * flight log, about a dozen; a power cut can leave a second copy of one. */
#define CACHE_RECORDS 48

#define PAGE_MAX 256u
#define CHUNK 64u

/* What alfs's format 6 keeps in a page beside its data: the header, the byte
 * count and the check; and on a file's first page its frame block. */
#define PAGE_KEEPS 20u
#define FIRST_PAGE_KEEPS 71u

static alfs_fs_t store;
static alfs_rec_t cache[CACHE_RECORDS];
static uint8_t scratch[PAGE_MAX];
static const alfs_area_t *area_of;
static const alfs_driver_t *drv_of;
static bool mounted, writable;

/* Handles open, which a mount would forget. */
static uint8_t held;

/* Writers whose close failed: alfs holds each open [WEB-API-18]. */
#define FAILED_NAMES 4
static struct {
    char name[ALFS_NAME_MAX + 1];
    bool remove; /* asked for while it waited */
} failed[FAILED_NAMES];
static uint8_t nfailed;

/* Pages a replaced or removed file left may wait for the cleanup. */
static bool dirty = true;

/* A rename's reader and writer, and a size's reader, under the lock. */
static uint8_t copy_rblk[PAGE_MAX], copy_wblk[PAGE_MAX];

static int err(int rc) {
    if (rc == ALFS_ENOENT)
        return VFS_FLASH_NOENT;
    if (rc == ALFS_ENOSPC)
        return VFS_FLASH_NOSPC;
    if (rc == ALFS_EOPEN)
        return VFS_FLASH_BUSY;
    return rc < 0 ? VFS_FLASH_ERR : rc;
}

int vfs_flash_mount(const alfs_area_t *area, const alfs_driver_t *drv, bool w) {
    if (!mounted) {
        if (area->page_size > PAGE_MAX || !fs_lock())
            return VFS_FLASH_ERR;
        int rc = alfs_fs_mount(&store, area, drv, scratch, cache, CACHE_RECORDS);
        fs_unlock();
        if (rc != ALFS_OK)
            return VFS_FLASH_ERR;
        area_of = area;
        drv_of = drv;
        mounted = true;
    }
    writable = writable || w;
    return 0;
}

bool vfs_flash_mounted(void) {
    return mounted;
}

static void fail(const char *name, bool remove) {
    if (nfailed < FAILED_NAMES) {
        memcpy(failed[nfailed].name, name, strlen(name) + 1);
        failed[nfailed].remove = remove;
    }
    nfailed++;
}

static bool remove_when_let_go(const char *name) {
    for (uint8_t i = 0; i < nfailed && i < FAILED_NAMES; i++) {
        if (strcmp(failed[i].name, name) == 0) {
            failed[i].remove = true;
            return true;
        }
    }
    return false;
}

/* The failed writers let go as a restart would: a mount, which finds every
 * file closed, then the removes asked for. A mount that fails leaves the
 * store unmounted, every call refused, until the next tries again. */
static void let_go(void) {
    if (!nfailed || held)
        return;
    memset(&store, 0, sizeof(store));
    if (alfs_fs_mount(&store, area_of, drv_of, scratch, cache, CACHE_RECORDS) != ALFS_OK)
        return;
    for (uint8_t i = 0; i < nfailed && i < FAILED_NAMES; i++) {
        if (failed[i].remove)
            alfs_file_delete(&store, failed[i].name);
    }
    nfailed = 0;
    dirty = true;
}

static int open_write(vfs_flash_file_t *f, const char *name, int mode) {
    int rc = ALFS_ENOENT;
    if (mode == VFS_FLASH_APPEND)
        rc = alfs_file_open(&store, &f->u.w, name, ALFS_GROW, f->buf, VFS_FLASH_BUF_SIZE);
    if (rc == ALFS_ENOENT) {
        uint32_t id;
        rc = alfs_file_alloc(&store, name, 0, &id);
        if (rc == ALFS_OK)
            rc = alfs_file_open(&store, &f->u.w, name, ALFS_GROW, f->buf, VFS_FLASH_BUF_SIZE);
        dirty = true; /* the allocation blocks out a copy a reset left open */
    }
    return rc;
}

int vfs_flash_open(vfs_flash_file_t *f, const char *name, int mode, void *buf) {
    size_t n = strlen(name);
    f->mode = 0;
    if (!mounted || n == 0 || n > ALFS_NAME_MAX || (mode != VFS_FLASH_READ && !writable))
        return VFS_FLASH_ERR;
    memcpy(f->name, name, n + 1);
    f->buf = buf;
    f->written = 0;
    if (!fs_lock())
        return VFS_FLASH_ERR;
    int rc = mode == VFS_FLASH_READ ? alfs_file_open_read(&store, &f->u.r, name, buf, VFS_FLASH_BUF_SIZE)
                                    : open_write(f, name, mode);
    if (rc == ALFS_OK)
        held++;
    fs_unlock();
    if (rc != ALFS_OK)
        return err(rc);
    f->mode = mode == VFS_FLASH_READ ? VFS_FLASH_READ : VFS_FLASH_WRITE;
    return 0;
}

/* Across pages until n or the end, as a caller reading a whole file in one
 * call expects: alfs gives one page's data a call. */
int vfs_flash_read(vfs_flash_file_t *f, void *buf, uint32_t n) {
    if (f->mode != VFS_FLASH_READ || !fs_lock())
        return VFS_FLASH_ERR;
    uint32_t got = 0;
    int32_t k = 1;
    while (got < n && k > 0) {
        k = alfs_file_read(&store, &f->u.r, (uint8_t *)buf + got, n - got);
        if (k > 0)
            got += (uint32_t)k;
    }
    fs_unlock();
    return k < 0 ? VFS_FLASH_ERR : (int)got;
}

int vfs_flash_write(vfs_flash_file_t *f, const void *buf, uint32_t n) {
    if (f->mode != VFS_FLASH_WRITE || !fs_lock())
        return VFS_FLASH_ERR;
    int32_t k = alfs_file_write(&store, &f->u.w, buf, n);
    fs_unlock();
    if (k < 0)
        return err((int)k);
    f->written += (uint32_t)k;
    return (int)k;
}

int vfs_flash_sync(vfs_flash_file_t *f) {
    if (f->mode != VFS_FLASH_WRITE || !fs_lock())
        return VFS_FLASH_ERR;
    int rc = alfs_file_flush(&store, &f->u.w);
    fs_unlock();
    return err(rc);
}

/* A close that finds no page for its last bytes gets one from the cleanup. */
#define CLOSE_CLEANUPS 4

int vfs_flash_close(vfs_flash_file_t *f) {
    if (!f->mode || !fs_lock())
        return VFS_FLASH_ERR;
    int rc;
    if (f->mode == VFS_FLASH_READ) {
        rc = alfs_file_close_read(&store, &f->u.r);
    } else {
        rc = alfs_file_close(&store, &f->u.w);
        for (int i = 0; i < CLOSE_CLEANUPS && rc == ALFS_ENOSPC && alfs_fs_cleanup(&store, 1) == ALFS_OK; i++)
            rc = alfs_file_close(&store, &f->u.w);
        if (rc != ALFS_OK)
            fail(f->name, false);
        dirty = true;
    }
    f->mode = 0;
    held--;
    let_go();
    fs_unlock();
    return err(rc);
}

/* Bytes from where reader r stands to the end of a file not closed. */
static int32_t count_rest(alfs_reader_t *r) {
    uint8_t chunk[CHUNK];
    int32_t total = 0, k;
    while ((k = alfs_file_read(&store, r, chunk, sizeof(chunk))) > 0)
        total += k;
    return k < 0 ? VFS_FLASH_ERR : total;
}

static int reopen(vfs_flash_file_t *f) {
    alfs_file_close_read(&store, &f->u.r);
    int rc = alfs_file_open_read(&store, &f->u.r, f->name, f->buf, VFS_FLASH_BUF_SIZE);
    if (rc != ALFS_OK) {
        f->mode = 0;
        held--;
    }
    return rc;
}

int32_t vfs_flash_size(vfs_flash_file_t *f) {
    if (f->mode == VFS_FLASH_WRITE)
        return (int32_t)f->written;
    if (f->mode != VFS_FLASH_READ)
        return VFS_FLASH_ERR;
    if (f->u.r.size != ALFS_SIZE_OPEN)
        return (int32_t)f->u.r.size;
    /* A file a reset left open records no size: its pages are counted, and
     * the reader goes back to the start. */
    if (!fs_lock())
        return VFS_FLASH_ERR;
    int32_t size = reopen(f) == ALFS_OK ? count_rest(&f->u.r) : VFS_FLASH_ERR;
    if (f->mode && reopen(f) != ALFS_OK)
        size = VFS_FLASH_ERR;
    fs_unlock();
    return size;
}

int vfs_flash_rewind(vfs_flash_file_t *f) {
    if (f->mode != VFS_FLASH_READ || !fs_lock())
        return VFS_FLASH_ERR;
    int rc = reopen(f);
    fs_unlock();
    return err(rc);
}

int vfs_flash_remove(const char *name) {
    if (!writable || !fs_lock())
        return VFS_FLASH_ERR;
    int rc = ALFS_OK;
    if (remove_when_let_go(name))
        let_go();
    else
        rc = alfs_file_delete(&store, name);
    dirty = true;
    fs_unlock();
    return err(rc);
}

/* `to` has a closed copy, which a copy not closed leaves as it is. */
static bool has_closed(const char *to) {
    alfs_reader_t r;
    if (alfs_file_open_read(&store, &r, to, copy_wblk, sizeof(copy_wblk)) != ALFS_OK)
        return false;
    bool closed = r.size != ALFS_SIZE_OPEN;
    alfs_file_close_read(&store, &r);
    return closed;
}

/*
 * A closed file's bytes into a new copy of `to`, reserved to its size. A copy
 * that comes up short is never closed, since closing it would replace `to`
 * with the part: it fails, and goes when let go unless `to` had a closed
 * copy, which reads as before. Only a damaged page in `from` cuts it short,
 * since the reservation holds every byte.
 */
static int copy(const char *from, const char *to) {
    alfs_reader_t r;
    alfs_file_t w;
    uint32_t id;
    int rc = alfs_file_open_read(&store, &r, from, copy_rblk, sizeof(copy_rblk));
    if (rc != ALFS_OK)
        return rc;
    bool replaces = has_closed(to);
    if (r.size == ALFS_SIZE_OPEN)
        rc = ALFS_EINVAL;
    if (rc == ALFS_OK)
        rc = alfs_file_alloc(&store, to, r.size, &id);
    if (rc == ALFS_OK)
        rc = alfs_file_open(&store, &w, to, 0, copy_wblk, sizeof(copy_wblk));
    bool opened = rc == ALFS_OK;
    uint32_t moved = 0;
    while (rc == ALFS_OK && moved < r.size) {
        uint8_t chunk[CHUNK];
        int32_t k = alfs_file_read(&store, &r, chunk, sizeof(chunk));
        if (k <= 0)
            rc = k < 0 ? (int)k : ALFS_ECHECK;
        else if (alfs_file_write(&store, &w, chunk, (uint32_t)k) != k)
            rc = ALFS_ENOSPC;
        else
            moved += (uint32_t)k;
    }
    if (rc == ALFS_OK && opened)
        rc = alfs_file_close(&store, &w);
    if (rc != ALFS_OK && opened)
        fail(to, !replaces);
    alfs_file_close_read(&store, &r);
    return rc;
}

int vfs_flash_rename(const char *from, const char *to) {
    if (!writable || strlen(to) > ALFS_NAME_MAX || !fs_lock())
        return VFS_FLASH_ERR;
    int rc = copy(from, to);
    if (rc == ALFS_OK)
        rc = alfs_file_delete(&store, from);
    dirty = true;
    let_go();
    fs_unlock();
    return err(rc);
}

int32_t vfs_flash_stat_size(const char *name) {
    if (!mounted || !fs_lock())
        return VFS_FLASH_ERR;
    alfs_reader_t r;
    int rc = alfs_file_open_read(&store, &r, name, copy_rblk, sizeof(copy_rblk));
    int32_t size = err(rc);
    if (rc == ALFS_OK) {
        size = r.size != ALFS_SIZE_OPEN ? (int32_t)r.size : count_rest(&r);
        alfs_file_close_read(&store, &r);
    }
    fs_unlock();
    return size;
}

/* The pages file info holds: a closed file's count, or, for one a reset left
 * open, its bytes counted and set in pages. */
static uint32_t pages_of(const alfs_file_info_t *info) {
    if (info->closed)
        return info->pages;
    uint32_t first = area_of->page_size - FIRST_PAGE_KEEPS, rest = area_of->page_size - PAGE_KEEPS;
    alfs_reader_t r;
    int32_t size = 0;
    if (alfs_file_open_read(&store, &r, info->name, copy_rblk, sizeof(copy_rblk)) == ALFS_OK) {
        size = count_rest(&r);
        alfs_file_close_read(&store, &r);
    }
    if (size <= (int32_t)first)
        return 1u;
    return 1u + ((uint32_t)size - first + rest - 1u) / rest;
}

int vfs_flash_space(uint64_t *free_bytes, uint64_t *total_bytes) {
    if (!mounted || !fs_lock())
        return VFS_FLASH_ERR;
    uint32_t pages = area_of->size / area_of->page_size - 1u, page = area_of->page_size - PAGE_KEEPS;
    uint32_t held = 0, after = 0;
    alfs_file_info_t info;
    while (alfs_file_list(&store, after, &info) == ALFS_OK) {
        after = info.id;
        held += pages_of(&info);
    }
    fs_unlock();
    *total_bytes = (uint64_t)pages * page;
    *free_bytes = (uint64_t)(held < pages ? pages - held : 0u) * page;
    return 0;
}

uint32_t vfs_flash_synced_cost(uint32_t bytes) {
    if (!area_of)
        return bytes;
    uint32_t page = area_of->page_size - PAGE_KEEPS;
    return (bytes + page - 1u) / page * page;
}

int vfs_flash_clean(void) {
    if (!writable || (!dirty && !nfailed))
        return 0;
    if (!fs_try_lock())
        return 1;
    let_go();
    int rc = alfs_fs_cleanup(&store, 1);
    if (rc == ALFS_CLEAN)
        dirty = false;
    fs_unlock();
    if (rc == ALFS_CLEAN)
        return 0;
    return (rc == ALFS_OK || rc == ALFS_EOPEN) ? 1 : VFS_FLASH_ERR;
}
