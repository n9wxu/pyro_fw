/*
 * fat_mimic - FAT12 USB Mass Storage over littlefs
 *
 * Three-layer architecture:
 *   USB layer (callbacks): RAM-only, no flash writes
 *   Sync layer (poll):     One file per call to littlefs
 *   Boot layer (init):     Build FAT image from littlefs
 *
 * Copyright 2025. BSD-3-Clause.
 */
#include "fat_mimic.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <tusb.h>
#include <pico/stdlib.h>

#ifndef FAT_MIMIC_DEBUG
#define FAT_MIMIC_DEBUG 0
#endif

#if FAT_MIMIC_DEBUG
#define DBG(fmt, ...) do { \
    char _b[80]; snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    uart_puts(uart0, _b); \
} while(0)
#else
#define DBG(...) ((void)0)
#endif

/* ── Geometry ─────────────────────────────────────────────────────── */

#define SECTOR          512
#define MBR_OFFSET      63
#define PART_SECTORS    1323
#define TOTAL_SECTORS   (MBR_OFFSET + PART_SECTORS)
#define FAT_SECTORS     4
#define ROOT_DIR_SECTORS 32
#define ROOT_DIR_ENTRIES 512
#define DATA_START      (1 + FAT_SECTORS + ROOT_DIR_SECTORS)
#define MAX_CLUSTERS    (PART_SECTORS - DATA_START + 2)

/* ── FAT directory entry ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat_dir_entry_t;

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

/* ── File table ───────────────────────────────────────────────────── */

#define MAX_FILES 16

typedef struct {
    uint16_t start_cluster;
    uint32_t size;
    char     path[28];
} file_entry_t;

static file_entry_t file_table[MAX_FILES];
static size_t       file_count;

/* ── RAM caches ───────────────────────────────────────────────────── */

static uint8_t boot_sector[SECTOR];
static uint8_t fat_cache[FAT_SECTORS * SECTOR];
static uint8_t root_dir[ROOT_DIR_SECTORS * SECTOR];

/* ── Dirty write-back cache ───────────────────────────────────────── */

#define MAX_DIRTY 32

typedef struct {
    uint16_t cluster;
    uint8_t  data[SECTOR];
} dirty_slot_t;

static dirty_slot_t dirty[MAX_DIRTY];
static volatile bool sync_needed;
static volatile bool dir_write_pending;
static volatile uint32_t last_usb_activity;

/* ── MBR ──────────────────────────────────────────────────────────── */

static const uint8_t mbr[SECTOR] = {
    [0x1BE] = 0x00, 0x01, 0x01, 0x00,
              0x0B, 0xFE, 0xFF, 0xFF,
              0x3F, 0x00, 0x00, 0x00,
              0x2B, 0x05, 0x00, 0x00,
    [0x1FE] = 0x55, [0x1FF] = 0xAA,
};

/* ── littlefs handle ──────────────────────────────────────────────── */

static const struct lfs_config *lfs_cfg;
static lfs_t lfs;

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint16_t fat12_get(uint16_t cluster) {
    uint32_t off = (cluster * 3) / 2;
    if (off + 1 >= sizeof(fat_cache)) return 0xFFF;
    if (cluster & 1)
        return ((fat_cache[off] >> 4) & 0x0F) | (fat_cache[off+1] << 4);
    else
        return fat_cache[off] | ((fat_cache[off+1] & 0x0F) << 8);
}

static void fat12_set(uint16_t cluster, uint16_t value) {
    uint32_t off = (cluster * 3) / 2;
    if (off + 1 >= sizeof(fat_cache)) return;
    if (cluster & 1) {
        fat_cache[off]   = (fat_cache[off] & 0x0F) | ((value & 0x0F) << 4);
        fat_cache[off+1] = (value >> 4) & 0xFF;
    } else {
        fat_cache[off]   = value & 0xFF;
        fat_cache[off+1] = (fat_cache[off+1] & 0xF0) | ((value >> 8) & 0x0F);
    }
}

static void name_to_83(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (base_len > 8) base_len = 8;
    for (int i = 0; i < base_len; i++)
        out[i] = toupper((unsigned char)name[i]);
    if (dot) {
        dot++;
        for (int i = 0; i < 3 && dot[i]; i++)
            out[8+i] = toupper((unsigned char)dot[i]);
    }
}

static void name_from_83(const uint8_t n[11], char *out) {
    int j = 0;
    for (int i = 0; i < 8 && n[i] != ' '; i++)
        out[j++] = n[i];
    if (n[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && n[i] != ' '; i++)
            out[j++] = n[i];
    }
    out[j] = '\0';
}

static file_entry_t *file_by_cluster(uint16_t cluster) {
    for (size_t i = 0; i < file_count; i++) {
        file_entry_t *f = &file_table[i];
        uint16_t nc = (f->size + SECTOR - 1) / SECTOR;
        if (nc == 0) nc = 1;
        if (cluster >= f->start_cluster && cluster < f->start_cluster + nc)
            return f;
    }
    return NULL;
}

static dirty_slot_t *dirty_find(uint16_t cluster) {
    for (int i = 0; i < MAX_DIRTY; i++)
        if (dirty[i].cluster == cluster)
            return &dirty[i];
    return NULL;
}

/* ── Boot sector generation ───────────────────────────────────────── */

static void build_boot_sector(void) {
    memset(boot_sector, 0, SECTOR);
    uint8_t *b = boot_sector;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b+3, "MSDOS5.0", 8);
    b[11] = 0x00; b[12] = 0x02;
    b[13] = 0x01;
    b[14] = 0x01; b[15] = 0x00;
    b[16] = 0x01;
    b[17] = ROOT_DIR_ENTRIES & 0xFF;
    b[18] = (ROOT_DIR_ENTRIES >> 8) & 0xFF;
    b[19] = PART_SECTORS & 0xFF;
    b[20] = (PART_SECTORS >> 8) & 0xFF;
    b[21] = 0xF8;
    b[22] = FAT_SECTORS; b[23] = 0x00;
    b[24] = 0x20; b[25] = 0x00;
    b[26] = 0x10; b[27] = 0x00;
    b[38] = 0x29;
    b[39] = 0x34; b[40] = 0x12;
    memcpy(b+43, "littlefsUSB", 11);
    memcpy(b+54, "FAT12   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

/* ── Build FAT image from littlefs ────────────────────────────────── */

static void build_from_littlefs(void) {
    memset(fat_cache, 0, sizeof(fat_cache));
    memset(root_dir, 0, sizeof(root_dir));
    memset(file_table, 0, sizeof(file_table));
    memset(dirty, 0, sizeof(dirty));
    file_count = 0;
    sync_needed = false;
    dir_write_pending = false;

    fat_cache[0] = 0xF8; fat_cache[1] = 0xFF; fat_cache[2] = 0xFF;

    fat_dir_entry_t *vol = (fat_dir_entry_t *)root_dir;
    memcpy(vol->name, "littlefsUSB", 11);
    vol->attr = ATTR_VOLUME_ID;

    lfs_dir_t dir;
    if (lfs_dir_open(&lfs, &dir, "/") < 0) return;

    struct lfs_info info;
    uint16_t next_cluster = 2;
    int dir_idx = 1;

    while (lfs_dir_read(&lfs, &dir, &info) > 0) {
        if (info.type != LFS_TYPE_REG) continue;
        if (info.name[0] == '.') continue;
        if (file_count >= MAX_FILES || dir_idx >= ROOT_DIR_ENTRIES) break;

        file_entry_t *fe = &file_table[file_count];
        fe->start_cluster = next_cluster;
        fe->size = info.size;
        snprintf(fe->path, sizeof(fe->path), "/%s", info.name);

        fat_dir_entry_t *de = (fat_dir_entry_t *)root_dir + dir_idx;
        name_to_83(info.name, de->name);
        de->attr = ATTR_ARCHIVE;
        de->fst_clus_lo = next_cluster;
        de->file_size = info.size;
        de->wrt_date = ((2025-1980) << 9) | (1 << 5) | 1;
        de->crt_date = de->wrt_date;

        uint16_t nc = (info.size > 0) ? (info.size + SECTOR - 1) / SECTOR : 1;
        for (uint16_t c = 0; c < nc; c++) {
            fat12_set(next_cluster + c, (c == nc - 1) ? 0xFFF : next_cluster + c + 1);
        }
        next_cluster += nc;
        file_count++;
        dir_idx++;
        DBG("  file: %s clus=%u sz=%lu\r\n", fe->path, fe->start_cluster, (unsigned long)fe->size);
    }
    lfs_dir_close(&lfs, &dir);
    DBG("built cache: %u files\r\n", (unsigned)file_count);
}

/* ── Forward declarations ─────────────────────────────────────────── */

static bool flush_one_file(void);
static bool delete_one_orphan(void);

/* ── Public API ───────────────────────────────────────────────────── */

void fat_mimic_init(const struct lfs_config *cfg) {
    lfs_cfg = cfg;
    int err = lfs_mount(&lfs, lfs_cfg);
    if (err < 0) {
        lfs_format(&lfs, lfs_cfg);
        lfs_mount(&lfs, lfs_cfg);
    }
}

void fat_mimic_mount(void) {
    build_boot_sector();
    build_from_littlefs();
}

void fat_mimic_unmount(void) {
    while (flush_one_file()) {}
    while (delete_one_orphan()) {}
}

uint32_t fat_mimic_capacity(void) {
    return TOTAL_SECTORS;
}

/* ── Read ─────────────────────────────────────────────────────────── */

void fat_mimic_read(uint8_t lun, uint32_t lba, void *buf, uint32_t len) {
    (void)lun;
    last_usb_activity = to_ms_since_boot(get_absolute_time());

    if (lba == 0) { memcpy(buf, mbr, len); return; }
    if (lba < MBR_OFFSET) { memset(buf, 0, len); return; }

    uint32_t ps = lba - MBR_OFFSET;

    if (ps == 0) { memcpy(buf, boot_sector, len); return; }

    if (ps >= 1 && ps < 1 + FAT_SECTORS) {
        memcpy(buf, fat_cache + (ps - 1) * SECTOR, len);
        return;
    }

    uint32_t rd_start = 1 + FAT_SECTORS;
    if (ps >= rd_start && ps < rd_start + ROOT_DIR_SECTORS) {
        memcpy(buf, root_dir + (ps - rd_start) * SECTOR, len);
        return;
    }

    uint32_t cluster = (ps - DATA_START) + 2;

    dirty_slot_t *ds = dirty_find(cluster);
    if (ds) { memcpy(buf, ds->data, len); return; }

    file_entry_t *f = file_by_cluster(cluster);
    if (!f) { memset(buf, 0, len); return; }

    uint32_t offset = (cluster - f->start_cluster) * SECTOR;
    lfs_file_t file;
    if (lfs_file_open(&lfs, &file, f->path, LFS_O_RDONLY) < 0) {
        memset(buf, 0, len);
        return;
    }
    lfs_file_seek(&lfs, &file, offset, LFS_SEEK_SET);
    lfs_ssize_t n = lfs_file_read(&lfs, &file, buf, len);
    if (n < (lfs_ssize_t)len)
        memset((uint8_t *)buf + (n > 0 ? n : 0), 0, len - (n > 0 ? n : 0));
    lfs_file_close(&lfs, &file);
}

/* ── Write ────────────────────────────────────────────────────────── */

void fat_mimic_write(uint8_t lun, uint32_t lba, void *buf, uint32_t len) {
    (void)lun;
    last_usb_activity = to_ms_since_boot(get_absolute_time());

    if (lba < MBR_OFFSET) return;
    uint32_t ps = lba - MBR_OFFSET;
    if (ps == 0) return;

    if (ps >= 1 && ps < 1 + FAT_SECTORS) {
        if (dir_write_pending) { sync_needed = true; dir_write_pending = false; }
        memcpy(fat_cache + (ps - 1) * SECTOR, buf, len);
        return;
    }

    uint32_t rd_start = 1 + FAT_SECTORS;
    if (ps >= rd_start && ps < rd_start + ROOT_DIR_SECTORS) {
        fat_dir_entry_t *old_ent = (fat_dir_entry_t *)(root_dir + (ps - rd_start) * SECTOR);
        fat_dir_entry_t *new_ent = (fat_dir_entry_t *)buf;
        for (int i = 0; i < SECTOR / 32; i++) {
            if (old_ent[i].name[0] == 0xE5 && new_ent[i].name[0] != 0xE5 && new_ent[i].name[0] != 0x00)
                sync_needed = true;
        }
        memcpy(root_dir + (ps - rd_start) * SECTOR, buf, len);
        dir_write_pending = true;
        if (ps == rd_start + ROOT_DIR_SECTORS - 1) {
            sync_needed = true;
            dir_write_pending = false;
        }
        return;
    }

    uint32_t cluster = (ps - DATA_START) + 2;
    if (dir_write_pending) { sync_needed = true; dir_write_pending = false; }
    DBG("W data clus=%lu\r\n", (unsigned long)cluster);

    for (int i = 0; i < MAX_DIRTY; i++) {
        if (dirty[i].cluster == cluster) {
            memcpy(dirty[i].data, buf, SECTOR);
            return;
        }
    }

    for (int i = 0; i < MAX_DIRTY; i++) {
        if (dirty[i].cluster == 0) {
            dirty[i].cluster = cluster;
            memcpy(dirty[i].data, buf, SECTOR);
            return;
        }
    }

    /* Cache full - evict slot 0 (backpressure) */
    file_entry_t *f = file_by_cluster(dirty[0].cluster);
    if (f) {
        lfs_file_t file;
        if (lfs_file_open(&lfs, &file, f->path, LFS_O_RDWR | LFS_O_CREAT) >= 0) {
            uint32_t off = (dirty[0].cluster - f->start_cluster) * SECTOR;
            lfs_file_seek(&lfs, &file, off, LFS_SEEK_SET);
            lfs_file_write(&lfs, &file, dirty[0].data, SECTOR);
            lfs_file_close(&lfs, &file);
        }
    }
    dirty[0].cluster = cluster;
    memcpy(dirty[0].data, buf, SECTOR);
    sync_needed = true;
}

/* ── Poll / Sync ──────────────────────────────────────────────────── */

static bool chain_has_dirty(uint16_t start) {
    uint16_t c = start;
    while (c >= 2 && c < 0xFF0) {
        if (dirty_find(c)) return true;
        uint16_t next = fat12_get(c);
        if (next >= 0xFF8) break;
        c = next;
    }
    return false;
}

static bool flush_one_file(void) {
    fat_dir_entry_t *entries = (fat_dir_entry_t *)root_dir;

    for (int i = 0; i < ROOT_DIR_ENTRIES; i++) {
        fat_dir_entry_t *de = &entries[i];
        if (de->name[0] == 0x00) break;
        if (de->name[0] == 0xE5) continue;
        if ((de->attr & ATTR_LFN) == ATTR_LFN) continue;
        if (de->attr & ATTR_VOLUME_ID) continue;
        if (de->attr & ATTR_DIRECTORY) continue;

        uint16_t start = de->fst_clus_lo;
        if (start < 2) continue;
        if (!chain_has_dirty(start)) continue;

        char name83[13];
        name_from_83(de->name, name83);
        char path[32];
        snprintf(path, sizeof(path), "/%s", name83);

        lfs_file_t file;
        if (lfs_file_open(&lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0)
            continue;

        uint16_t c = start;
        while (c >= 2 && c < 0xFF0) {
            dirty_slot_t *ds = dirty_find(c);
            if (ds) {
                lfs_file_write(&lfs, &file, ds->data, SECTOR);
                ds->cluster = 0;
            } else {
                file_entry_t *fe = file_by_cluster(c);
                uint8_t tmp[SECTOR] = {0};
                if (fe) {
                    lfs_file_t src;
                    if (lfs_file_open(&lfs, &src, fe->path, LFS_O_RDONLY) >= 0) {
                        lfs_file_seek(&lfs, &src, (c - fe->start_cluster) * SECTOR, LFS_SEEK_SET);
                        lfs_ssize_t n = lfs_file_read(&lfs, &src, tmp, SECTOR);
                        (void)n;
                        lfs_file_close(&lfs, &src);
                    }
                }
                lfs_file_write(&lfs, &file, tmp, SECTOR);
            }
            uint16_t next = fat12_get(c);
            if (next >= 0xFF8) break;
            c = next;
        }

        if (de->file_size > 0)
            lfs_file_truncate(&lfs, &file, de->file_size);
        lfs_file_close(&lfs, &file);
        DBG("FLUSH %s clus=%u sz=%lu\r\n", path, start, (unsigned long)de->file_size);

        bool found = false;
        for (size_t j = 0; j < file_count; j++) {
            if (file_table[j].start_cluster == start) {
                file_table[j].size = de->file_size;
                found = true;
                break;
            }
        }
        if (!found && file_count < MAX_FILES) {
            file_table[file_count].start_cluster = start;
            file_table[file_count].size = de->file_size;
            snprintf(file_table[file_count].path, sizeof(file_table[file_count].path), "%s", path);
            file_count++;
        }
        return true;
    }
    return false;
}

static bool delete_one_orphan(void) {
    fat_dir_entry_t *entries = (fat_dir_entry_t *)root_dir;

    for (size_t j = 0; j < file_count; j++) {
        bool found = false;
        for (int i = 0; i < ROOT_DIR_ENTRIES && !found; i++) {
            fat_dir_entry_t *de = &entries[i];
            if (de->name[0] == 0x00) break;
            if (de->name[0] == 0xE5) continue;
            if ((de->attr & ATTR_LFN) == ATTR_LFN) continue;
            if (de->attr & ATTR_VOLUME_ID) continue;
            if (de->attr & ATTR_DIRECTORY) continue;
            if (de->fst_clus_lo == file_table[j].start_cluster)
                found = true;
        }
        if (!found) {
            lfs_remove(&lfs, file_table[j].path);
            DBG("DEL %s\r\n", file_table[j].path);
            file_table[j] = file_table[file_count - 1];
            file_count--;
            return true;
        }
    }
    return false;
}

void fat_mimic_poll(void) {
    if (!sync_needed) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_usb_activity < 50) return;

    if (flush_one_file()) return;
    sync_needed = false;
}
