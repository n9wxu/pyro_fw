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
#include "pico/stdlib.h"

/* Every program and erase runs under flash_op()'s lockout, which parks the
 * other core and raises the caller to T for the operation alone. A refused
 * operation -- from the flight task, or a helper that would not park --
 * returns LFS_ERR_IO, a failed file operation every caller already retries.
 *
 * LFS_THREADSAFE: littlefs takes lfs_lock() around every public call, so the
 * one mount (lfs_mount.h) serves every task but the flight task. */
#include "flash_op.h"
#include "lfs_mount.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "semphr.h"

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

static int pico_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;

    uint8_t *p = (uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + fs_base(c) + (block * FLASH_SECTOR_SIZE) + off);
    memcpy(buffer, p, size);
    return 0;
}

static int pico_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer,
                     lfs_size_t size) {
    uint32_t p = (block * FLASH_SECTOR_SIZE) + off;
    flash_op_crumb(97);
    int rc = flash_op_program(fs_base(c) + p, (const uint8_t *)buffer, size);
    flash_op_crumb(98);
    return rc == 0 ? 0 : LFS_ERR_IO;
}

static int pico_erase(const struct lfs_config *c, lfs_block_t block) {
    uint32_t off = block * FLASH_SECTOR_SIZE;
    flash_op_crumb(95);
    int rc = flash_op_erase(fs_base(c) + off, FLASH_SECTOR_SIZE);
    flash_op_crumb(96);
    return rc == 0 ? 0 : LFS_ERR_IO;
}

static int pico_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

/* Before the scheduler there is one thread, and the mutex does not exist. A
 * lock not taken in 10 s is a holder that is not coming back: the call fails
 * as an I/O error rather than hanging its task. Never the flight task. */
static SemaphoreHandle_t lfs_mutex;
static StaticSemaphore_t lfs_mutex_buf;

static int pico_lock(const struct lfs_config *c) {
    (void)c;
    if (!rtos_running())
        return 0;
    if (rtos_in_flight_task())
        return LFS_ERR_IO;
    if (!lfs_mutex)
        lfs_mutex = xSemaphoreCreateMutexStatic(&lfs_mutex_buf);
    return xSemaphoreTake(lfs_mutex, pdMS_TO_TICKS(10000)) == pdTRUE ? 0 : LFS_ERR_IO;
}

static int pico_unlock(const struct lfs_config *c) {
    (void)c;
    if (rtos_running() && lfs_mutex)
        xSemaphoreGive(lfs_mutex);
    return 0;
}

/* Static buffers for LFS_NO_MALLOC. Linking pico_multicore, which the
 * FreeRTOS port does, makes every malloc and free take a mutex, and no file
 * path should hold a task on the heap's lock. Each open file brings its own
 * cache buffer (lfs_mount.h). */
static uint8_t lfs_read_buf[LFS_FILE_BUF_SIZE];
static uint8_t lfs_prog_buf[LFS_FILE_BUF_SIZE];
static uint8_t lfs_lookahead_buf[16] __attribute__((aligned(8)));

const struct lfs_config lfs_pico_flash_config = {
    .read = pico_read,
    .prog = pico_prog,
    .erase = pico_erase,
    .sync = pico_sync,
    .lock = pico_lock,
    .unlock = pico_unlock,
    .read_size = 1,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = FS_SIZE / FLASH_SECTOR_SIZE,
    .cache_size = LFS_FILE_BUF_SIZE,
    .lookahead_size = 16,
    .block_cycles = 500,
    .read_buffer = lfs_read_buf,
    .prog_buffer = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead_buf,
};
