/*
 * The files, wherever they live. See vfs.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vfs.h"
#include "fs_flash.h"
#include "flight_resume.h"
#include "pin_store.h"
#include "beep_store.h"
#include <string.h>
#if PYRO_HAS_SD
#include "sd_card.h"
#endif

/* The board's, not the card's: the identity a host knows the board by, and
 * the marker a power-cut board recovers its flight from. */
static const char *const internal_paths[] = {"serial.txt", PAD_RECORD_PATH};

/* The configuration: the card's copy wins, and the flash keeps a mirror. */
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
    return VFS_FLASH;
}

int vfs_mount(bool writable) {
    return vfs_flash_mount(&fs_area, &fs_driver, writable);
}

int vfs_clean(void) {
    return vfs_flash_clean();
}

#if PYRO_HAS_SD
static int fat_err(FRESULT r) {
    return (r == FR_NO_FILE || r == FR_NO_PATH) ? VFS_NOENT : (r == FR_OK ? 0 : VFS_ERR);
}
#endif

static int open_on(int kind, vfs_file_t *f, const char *path, int flags, void *buf) {
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
    int mode = VFS_FLASH_READ;
    if (flags & VFS_WR)
        mode = (flags & VFS_APPEND) ? VFS_FLASH_APPEND : VFS_FLASH_WRITE;
    int rc = vfs_flash_open(&f->u.flash, bare(path), mode, buf);
    if (rc == 0)
        f->kind = VFS_FLASH;
    return rc;
}

/* A file the card lacks is read from the flash, so a blank card still serves
 * the web pages and the last flight the board kept. Writes go to the card. */
int vfs_open(vfs_file_t *f, const char *path, int flags, void *buf) {
    f->kind = VFS_NONE;
    if (!vfs_path_ok(path))
        return VFS_ERR;
    int kind = vfs_route(path);
    int rc = open_on(kind, f, path, flags, buf);
    if (rc == VFS_NOENT && kind == VFS_FAT && !(flags & VFS_WR))
        rc = open_on(VFS_FLASH, f, path, flags, buf);
    return rc;
}

int vfs_read(vfs_file_t *f, void *buf, uint32_t n) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT) {
        UINT got = 0;
        return f_read(&f->u.fat, buf, n, &got) == FR_OK ? (int)got : VFS_ERR;
    }
#endif
    if (f->kind != VFS_FLASH)
        return VFS_ERR;
    return vfs_flash_read(&f->u.flash, buf, n);
}

int vfs_write(vfs_file_t *f, const void *buf, uint32_t n) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT) {
        UINT put = 0;
        return f_write(&f->u.fat, buf, n, &put) == FR_OK ? (int)put : VFS_ERR;
    }
#endif
    if (f->kind != VFS_FLASH)
        return VFS_ERR;
    return vfs_flash_write(&f->u.flash, buf, n);
}

int vfs_sync(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return fat_err(f_sync(&f->u.fat));
#endif
    if (f->kind != VFS_FLASH)
        return VFS_ERR;
    return vfs_flash_sync(&f->u.flash);
}

int vfs_close(vfs_file_t *f) {
    int rc = VFS_ERR;
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        rc = fat_err(f_close(&f->u.fat));
#endif
    if (f->kind == VFS_FLASH)
        rc = vfs_flash_close(&f->u.flash);
    f->kind = VFS_NONE;
    return rc;
}

int32_t vfs_size(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return (int32_t)f_size(&f->u.fat);
#endif
    if (f->kind != VFS_FLASH)
        return VFS_ERR;
    return vfs_flash_size(&f->u.flash);
}

int vfs_rewind(vfs_file_t *f) {
#if PYRO_HAS_SD
    if (f->kind == VFS_FAT)
        return fat_err(f_lseek(&f->u.fat, 0));
#endif
    if (f->kind != VFS_FLASH)
        return VFS_ERR;
    return vfs_flash_rewind(&f->u.flash);
}

/* A read of a file the card lacks falls back to internal storage
 * (vfs_open), so a copy left there would come back: both go. */
int vfs_remove(const char *path) {
    if (!vfs_path_ok(path))
        return VFS_ERR;
    int internal = vfs_flash_remove(bare(path));
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        int card = fat_err(f_unlink(path));
        return card == VFS_NOENT ? internal : card;
    }
#endif
    return internal;
}

int vfs_rename(const char *from, const char *to) {
    if (!vfs_path_ok(from) || !vfs_path_ok(to))
        return VFS_ERR;
#if PYRO_HAS_SD
    if (vfs_route(to) == VFS_FAT) {
        /* FatFs will not rename over an existing file. */
        f_unlink(to);
        return fat_err(f_rename(from, to));
    }
#endif
    return vfs_flash_rename(bare(from), bare(to));
}

int vfs_mkdir(const char *path) {
    if (!vfs_path_ok(path))
        return VFS_ERR;
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        FRESULT r = f_mkdir(path);
        return (r == FR_OK || r == FR_EXIST) ? 0 : VFS_ERR;
    }
#endif
    return 0; /* the flash's names carry their own '/' */
}

int32_t vfs_stat_size(const char *path) {
    if (!vfs_path_ok(path))
        return VFS_ERR;
#if PYRO_HAS_SD
    if (vfs_route(path) == VFS_FAT) {
        FILINFO fi;
        FRESULT r = f_stat(path, &fi);
        if (r == FR_OK)
            return (int32_t)fi.fsize;
        if (fat_err(r) != VFS_NOENT)
            return VFS_ERR;
    }
#endif
    return vfs_flash_stat_size(bare(path));
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
    return vfs_flash_space(free_bytes, total_bytes);
}

/* ── Mirroring the configuration into the flash ────────────────── */

#if PYRO_HAS_SD
#define MIRROR_CHUNK 256

static uint8_t mirror_a[MIRROR_CHUNK], mirror_b[MIRROR_CHUNK];
static uint8_t mirror_buf[VFS_BUF_SIZE];

/* True when the card's file and the flash's differ, or the flash has none. */
static bool differs(const char *path) {
    vfs_file_t card, own;
    if (open_on(VFS_FAT, &card, path, VFS_RD, NULL) != 0)
        return false; /* nothing on the card: nothing to copy */
    bool diff = true;
    if (open_on(VFS_FLASH, &own, path, VFS_RD, mirror_buf) == 0) {
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
    bool ok = open_on(VFS_FLASH, &own, part, VFS_WR, mirror_buf) == 0;
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
        ok = vfs_flash_rename(part, bare(path)) == 0;
    if (!ok && opened)
        vfs_flash_remove(part);
    return ok;
}

/* The flash's copy onto the card, for a card that has none: a blank card in
 * a configured board takes the board's configuration rather than defaults. */
static bool copy_out(const char *path) {
    vfs_file_t own, card;
    if (open_on(VFS_FLASH, &own, path, VFS_RD, mirror_buf) != 0)
        return false;
    bool ok = open_on(VFS_FAT, &card, path, VFS_WR, NULL) == 0;
    bool opened = ok;
    while (ok) {
        int n = vfs_read(&own, mirror_a, MIRROR_CHUNK);
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        ok = vfs_write(&card, mirror_a, (uint32_t)n) == n;
    }
    if (opened)
        ok = (vfs_close(&card) == 0) && ok;
    vfs_close(&own);
    if (!ok && opened)
        f_unlink(path);
    return ok;
}

static bool on_card(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
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
    if (!vfs_sd_mounted())
        return 0;
    for (unsigned i = 0; i < sizeof(config_paths) / sizeof(config_paths[0]); i++) {
        const char *p = config_paths[i];
        if (on_card(p))
            n += vfs_mirror_one(p);
        else
            n += copy_out(p) ? 1 : 0;
    }
#endif
    return n;
}
