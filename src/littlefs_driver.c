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

/* Core1 must not be fetching instructions from flash while XIP is off, and
 * save_and_disable_interrupts() below acts on the calling core only.
 *
 * Core1 is a dispatched worker: it idles in a RAM-resident spin and executes
 * only the units core0 hands it. So core0 does not ask permission -- it
 * checks a flag core1 publishes from inside that RAM loop, which means
 * "core1's program counter is in RAM", not "core1 has promised to go there".
 *
 * Weak, and true, when Lua is not linked: there is no second core to collide
 * with. See docs/core1_hazard.md. */
__attribute__((weak)) bool lua_core1_idle(void) {
    return true;
}

/* An erase while core1 is mid-unit would stall it on a flash fetch and, on
 * the bench, took core0 with it. This is the assertion that cannot happen --
 * the caller only reaches flash through lfs, and lfs only runs where core0
 * has already established that core1 is idle. */
static void assert_core1_idle(void) {
    while (!lua_core1_idle()) {
        /* Deliberately not a wait for core1's cooperation: reaching here at
         * all means a flash write was started from somewhere that did not
         * check, which is a bug in the caller rather than a race to ride out.
         * Spinning makes it obvious instead of silently corrupting. */
        tight_loop_contents();
    }
}

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
    (void)c;
    uint32_t p = (block * FLASH_SECTOR_SIZE) + off;
    assert_core1_idle();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(fs_base(c) + p, buffer, size);
    restore_interrupts(ints);
    return 0;
}

static int pico_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    uint32_t off = block * FLASH_SECTOR_SIZE;
    assert_core1_idle();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(fs_base(c) + off, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 0;
}

static int pico_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

/* Static buffers for LFS_NO_MALLOC.
 *
 * littlefs is the ONLY caller of malloc in this firmware (lfs_init takes
 * three buffers, lfs_file_opencfg takes one). Linking pico_multicore -- which
 * Lua requires -- makes every malloc and free on both cores enter
 * malloc_mutex through mutex_enter_blocking(), a call with no timeout. That
 * put an unbounded wait on the liftoff path:
 *
 *     action_launch -> hal_log_start -> lfs_mount -> __wrap_free
 *                   -> mutex_enter_blocking
 *
 * Giving littlefs static buffers removes core0's last heap call, so the
 * mutex is never entered and the hazard does not exist rather than being
 * mitigated. support/prove_core0.py fails the build if it comes back.
 * See docs/core1_hazard.md. */
static uint8_t lfs_read_buf[FLASH_SECTOR_SIZE];
static uint8_t lfs_prog_buf[FLASH_SECTOR_SIZE];
static uint8_t lfs_lookahead_buf[16] __attribute__((aligned(8)));

const struct lfs_config lfs_pico_flash_config = {
    .read = pico_read,
    .prog = pico_prog,
    .erase = pico_erase,
    .sync = pico_sync,
    .read_size = 1,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = FS_SIZE / FLASH_SECTOR_SIZE,
    .cache_size = FLASH_SECTOR_SIZE,
    .lookahead_size = 16,
    .block_cycles = 500,
    .read_buffer = lfs_read_buf,
    .prog_buffer = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead_buf,
};

/* Per-file cache, likewise static. hal_fs_open() keeps a single streaming
 * file, and the short-lived helpers open one at a time, so one buffer and one
 * config are enough; hal_common.c serialises them through hw_file.open. */
static uint8_t lfs_file_buf[FLASH_SECTOR_SIZE];
const struct lfs_file_config lfs_pico_file_config = {
    .buffer = lfs_file_buf,
};
