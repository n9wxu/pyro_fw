/*
 * Driver for Raspberry Pi Pico on-board flash with littlefs file system
 *
 * Copyright 2024, Hiroyuki OYAMA. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <hardware/flash.h>
#include <hardware/sync.h>
#include <hardware/regs/addressmap.h>
#include <lfs.h>


/* Must match PFB_RESERVED_FILESYSTEM_SIZE_KB exactly: pico_fota_bootloader
 * carves the filesystem out of the top of flash and sizes its A/B slots
 * around it, while fs_base() below places littlefs at the top of flash by
 * working down from PICO_FLASH_SIZE_BYTES. If the two disagree, littlefs
 * either strands reserved space or -- worse -- runs down into the download
 * slot. CMake passes the single source of truth. */
#ifndef PYRO_FS_SIZE_KB
#error "PYRO_FS_SIZE_KB not defined - CMake must pass PFB_RESERVED_FILESYSTEM_SIZE_KB"
#endif

#define FS_SIZE (PYRO_FS_SIZE_KB * 1024)

_Static_assert(FS_SIZE % FLASH_SECTOR_SIZE == 0, "filesystem size must be a whole number of flash sectors");
_Static_assert(FS_SIZE < PICO_FLASH_SIZE_BYTES, "filesystem does not fit in flash");


static uint32_t fs_base(const struct lfs_config *c) {
    uint32_t storage_size = c->block_count * c->block_size;
    return PICO_FLASH_SIZE_BYTES - storage_size;
}

static int pico_read(const struct lfs_config* c,
                     lfs_block_t block,
                     lfs_off_t off,
                     void* buffer,
                     lfs_size_t size)
{
    (void)c;

    uint8_t* p = (uint8_t*)(XIP_NOCACHE_NOALLOC_BASE + fs_base(c) + (block * FLASH_SECTOR_SIZE) + off);
    memcpy(buffer, p, size);
    return 0;
}

static int pico_prog(const struct lfs_config* c,
                     lfs_block_t block,
                     lfs_off_t off,
                     const void* buffer,
                     lfs_size_t size)
{
    (void)c;
    uint32_t p = (block * FLASH_SECTOR_SIZE) + off;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(fs_base(c) + p, buffer, size);
    restore_interrupts(ints);
    return 0;
}

static int pico_erase(const struct lfs_config* c, lfs_block_t block) {
    (void)c;
    uint32_t off = block * FLASH_SECTOR_SIZE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(fs_base(c) + off, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 0;
}

static int pico_sync(const struct lfs_config* c) {
    (void)c;
    return 0;
}

const struct lfs_config lfs_pico_flash_config = {
    .read  = pico_read,
    .prog  = pico_prog,
    .erase = pico_erase,
    .sync  = pico_sync,
    .read_size      = 1,
    .prog_size      = FLASH_PAGE_SIZE,
    .block_size     = FLASH_SECTOR_SIZE,
    .block_count    = FS_SIZE / FLASH_SECTOR_SIZE,
    .cache_size     = FLASH_SECTOR_SIZE,
    .lookahead_size = 16,
    .block_cycles   = 500,
};
