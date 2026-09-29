/*
 * The files, wherever they live. See vfs.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vfs.h"
#include "lfs_mount.h"
#include "brownout.h"
#include "pin_store.h"
#include "beep_store.h"
#include <string.h>
#if PYRO_HAS_SD
#include "sd_card.h"
#endif

/* The board's, not the card's: the identity a host knows the board by, and
 * the marker a power-cut board recovers its flight from. */
static const char *const internal_paths[] = {"serial.txt", PAD_MARKER_PATH};

/* The configuration: the card's copy wins, and littlefs keeps a mirror. */
static const char *const config_paths[] = {"config.ini", PIN_STORE_PATH, BEEP_STORE_PATH, "lua_user.lua"};

static const char *bare(const char *path) {
    while (*path == '/')
        path++;
    return path;
}

static bool in_list(const char *path, const char *const *list, unsigned n) {
    const char *p = bare(path);
    for (unsigned i = 0; i < n; i++)
        if (strcmp(p, list[i]) == 0)
            return true;
    return false;
}

bool vfs_is_config(const char *path) {
    return in_list(path, config_paths, sizeof(config_paths) / sizeof(config_paths[0]));
}

bool vfs_sd_mounted(void) {
#if PYRO_HAS_SD
    return sd_mounted();
#else
    return false;
#endif
}

int vfs_route(const char *path) {
    if (vfs_sd_mounted() && !in_list(path, internal_paths, sizeof(internal_paths) / sizeof(internal_paths[0])))
        return VFS_FAT;
    return VFS_LFS;
}

static int lfs_err(int e) {
    return e == LFS_ERR_NOENT ? VFS_NOENT : (e < 0 ? VFS_ERR : e);
}

#if PYRO_HAS_SD
static int fat_err(FRESULT r) {
    return (r == FR_NO_FILE || r == FR_NO_PATH) ? VFS_NOENT : (r == FR_OK ? 0 : VFS_ERR);
}
#endif

static int open_on(int kind, vfs_file_t *f, const char *path, int flags, void *lfs_buf) {
    f->kind = VFS_NONE;
#if PYRO_HAS_SD
    if (kind == VFS_FAT) {
        BYTE mode = FA_READ;
        if (flags & VFS_WR)
            mode = (flags & VFS_APPEND) ? (FA_WRITE | FA_OPEN_APPEND) : (FA_WRITE | FA_CREATE_ALWAYS);
        FRESULT r = f_open(&f->u.fat, path, mode);
        if (r != FR_OK)
            return fat_err(r);
        f->kind = VFS_FAT;
        return 0;
    }
#endif
    (void)kind;
    int lf = LFS_O_RDONLY;
    if (flags & VFS_WR)
        lf = LFS_O_WRONLY | LFS_O_CREAT | ((flags & VFS_APPEND) ? LFS_O_APPEND : LFS_O_TRUNC);
    f->lcfg = (struct lfs_file_config){.buffer = lfs_buf};
    int e = lfs_file_opencfg(&g_lfs, &f->u.lfs, path, lf, &f->lcfg);
    if (e < 0)
        return lfs_err(e);
    f->kind = VFS_LFS;
    return 0;
}

int vfs_open(vfs_file_t *f, const char *path, int flags, void *lfs_buf) {
    return open_on(vfs_route(path), f, path, flags, lfs_buf);
}

int vfs_read(vfs_file_t *f, void *buf, uint32_t n) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT) {
        UINT got = 0;
        return f_read(&f->u.fat, buf, n, &got) == FR_OK ? (int)got : VFS_ERR;
    }
#endif
    if (f->kind != VFS_LFS)
        return VFS_ERR;
    return lfs_err((int)lfs_file_read(&g_lfs, &f->u.lfs, buf, n));
}

int vfs_write(vfs_file_t *f, const void *buf, uint32_t n) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT) {
        UINT put = 0;
        return f_write(&f->u.fat, buf, n, &put) == FR_OK ? (int)put : VFS_ERR;
    }
#endif
    if (f->kind != VFS_LFS)
        return VFS_ERR;
    return lfs_err((int)lfs_file_write(&g_lfs, &f->u.lfs, buf, n));
}

int vfs_sync(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return fat_err(f_sync(&f->u.fat));
#endif
    if (f->kind != VFS_LFS)
        return VFS_ERR;
    return lfs_err(lfs_file_sync(&g_lfs, &f->u.lfs));
}

int vfs_close(vfs_file_t *f) {
    int rc = VFS_ERR;
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        rc = fat_err(f_close(&f->u.fat));
#endif
    if (f->kind == VFS_LFS)
        rc = lfs_err(lfs_file_close(&g_lfs, &f->u.lfs));
    f->kind = VFS_NONE;
    return rc;
}

int32_t vfs_size(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return (int32_t)f_size(&f->u.fat);
#endif
    if (f->kind != VFS_LFS)
        return VFS_ERR;
    return lfs_err((int)lfs_file_size(&g_lfs, &f->u.lfs));
}

int vfs_rewind(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return fat_err(f_lseek(&f->u.fat, 0));
#endif
    if (f->kind != VFS_LFS)
        return VFS_ERR;
    return lfs_err(lfs_file_rewind(&g_lfs, &f->u.lfs));
}

int vfs_remove(const char *path) {
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT)
        return fat_err(f_unlink(path));
#endif
    return lfs_err(lfs_remove(&g_lfs, path));
}

int vfs_rename(const char *from, const char *to) {
#if PYRO_HAS_SD
    if (vfs_route(to) == VFS_FAT) {
        /* FatFs will not rename over an existing file; littlefs does. */
        f_unlink(to);
        return fat_err(f_rename(from, to));
    }
#endif
    return lfs_err(lfs_rename(&g_lfs, from, to));
}

int vfs_mkdir(const char *path) {
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        FRESULT r = f_mkdir(path);
        return (r == FR_OK || r == FR_EXIST) ? 0 : VFS_ERR;
    }
#endif
    int e = lfs_mkdir(&g_lfs, path);
    return (e == LFS_ERR_OK || e == LFS_ERR_EXIST) ? 0 : VFS_ERR;
}

int32_t vfs_stat_size(const char *path) {
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        FILINFO fi;
        FRESULT r = f_stat(path, &fi);
        return r == FR_OK ? (int32_t)fi.fsize : fat_err(r);
    }
#endif
    struct lfs_info info;
    int e = lfs_stat(&g_lfs, path, &info);
    return e < 0 ? lfs_err(e) : (int32_t)info.size;
}

int vfs_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes) {
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        FATFS *fs;
        DWORD free_clust;
        if (f_getfree("", &free_clust, &fs) != FR_OK)
            return VFS_ERR;
        uint64_t clust = (uint64_t)fs->csize * 512u;
        *free_bytes = (uint64_t)free_clust * clust;
        *total_bytes = (uint64_t)(fs->n_fatent - 2u) * clust;
        return 0;
    }
#endif
    (void)path;
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    if (used < 0)
        return VFS_ERR;
    uint64_t block = lfs_pico_flash_config.block_size;
    *total_bytes = (uint64_t)lfs_pico_flash_config.block_count * block;
    *free_bytes = (uint64_t)(lfs_pico_flash_config.block_count - (uint32_t)used) * block;
    return 0;
}

/* ── Mirroring the configuration into littlefs ─────────────────── */

#if PYRO_HAS_SD
#define MIRROR_CHUNK 256

static uint8_t mirror_a[MIRROR_CHUNK], mirror_b[MIRROR_CHUNK];
static uint8_t mirror_lfs_buf[LFS_FILE_BUF_SIZE];

/* True when the card's file and littlefs's differ, or littlefs has none. */
static bool differs(const char *path) {
    vfs_file_t card, own;
    if (open_on(VFS_FAT, &card, path, VFS_RD, NULL) != 0)
        return false; /* nothing on the card: nothing to copy */
    bool diff = true;
    if (open_on(VFS_LFS, &own, path, VFS_RD, mirror_lfs_buf) == 0) {
        diff = vfs_size(&card) != vfs_size(&own);
        while (!diff) {
            int a = vfs_read(&card, mirror_a, MIRROR_CHUNK);
            int b = vfs_read(&own, mirror_b, MIRROR_CHUNK);
            if (a != b || a < 0 || memcmp(mirror_a, mirror_b, (size_t)(a > 0 ? a : 0)) != 0)
                diff = true;
            else if (a == 0)
                break;
        }
        vfs_close(&own);
    }
    vfs_close(&card);
    return diff;
}

/* Into a part file renamed over the mirror only when whole, so a copy that
 * fails part-way leaves the old mirror as it was. */
static bool copy_in(const char *path) {
    char part[48];
    size_t len = strlen(bare(path));
    if (len + 6 > sizeof(part))
        return false;
    memcpy(part, bare(path), len);
    memcpy(part + len, ".part", 6);
    vfs_file_t card, own;
    if (open_on(VFS_FAT, &card, path, VFS_RD, NULL) != 0)
        return false;
    bool ok = open_on(VFS_LFS, &own, part, VFS_WR, mirror_lfs_buf) == 0;
    bool opened = ok;
    while (ok) {
        int n = vfs_read(&card, mirror_a, MIRROR_CHUNK);
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        ok = vfs_write(&own, mirror_a, (uint32_t)n) == n;
    }
    if (opened)
        ok = (vfs_close(&own) == 0) && ok;
    vfs_close(&card);
    if (ok)
        ok = lfs_rename(&g_lfs, part, bare(path)) == LFS_ERR_OK;
    if (!ok && opened)
        lfs_remove(&g_lfs, part);
    return ok;
}

#endif

int vfs_mirror_one(const char *path) {
#if PYRO_HAS_SD
    if (!vfs_sd_mounted() || !vfs_is_config(path) || !differs(path))
        return 0;
    return copy_in(path) ? 1 : 0;
#else
    (void)path;
    return 0;
#endif
}

int vfs_mirror(void) {
    int n = 0;
#if PYRO_HAS_SD
    for (unsigned i = 0; i < sizeof(config_paths) / sizeof(config_paths[0]); i++)
        n += vfs_mirror_one(config_paths[i]);
#endif
    return n;
}
