/*
 * The filesystem area on the boot flash, as alfs reaches it. See fs_flash.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fs_flash.h"
#include "fs_lock.h"
#include "flash_op.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <hardware/flash.h>
#include <hardware/regs/addressmap.h>
#include <string.h>

/* Must match PFB_RESERVED_FILESYSTEM_SIZE_KB exactly: pico_fota_bootloader
 * carves the filesystem out of the top of flash and sizes its A/B slots
 * around it. If the two disagree, the files either strand reserved space or
 * run down into the download slot. CMake passes the one number. */
#ifndef PYRO_FS_SIZE_KB
#error "PYRO_FS_SIZE_KB not defined - CMake must pass PFB_RESERVED_FILESYSTEM_SIZE_KB"
#endif

#define FS_SIZE (PYRO_FS_SIZE_KB * 1024u)

_Static_assert(FS_SIZE % FLASH_SECTOR_SIZE == 0, "filesystem size must be a whole number of flash sectors");
_Static_assert(FS_SIZE < PICO_FLASH_SIZE_BYTES, "filesystem does not fit in flash");

const alfs_area_t fs_area = {
    .start = PICO_FLASH_SIZE_BYTES - FS_SIZE,
    .size = FS_SIZE,
    .chip_size = PICO_FLASH_SIZE_BYTES,
    .sector_size = FLASH_SECTOR_SIZE,
    .page_size = FLASH_PAGE_SIZE,
};

static int fs_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    (void)ctx;
    memcpy(buf, (const void *)(XIP_NOCACHE_NOALLOC_BASE + addr), len);
    return 0;
}

/* flash_op() says no while a board's pyro sequence runs past its wait, and
 * when the other core's helper would not park. Neither is the chip's fault,
 * and alfs takes a failed program for one, so the driver asks again, up to a
 * deadline past any fire sequence [DD-056]. The flight task and interrupts
 * are refused for good, and never call here. */
#define REFUSAL_TRIES 8

static bool refused_for_now(int rc) {
    return rc == -2 || rc == -3;
}

static int fs_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    (void)ctx;
    int rc = -1;
    flash_op_crumb(97);
    for (int i = 0; i < REFUSAL_TRIES && (i == 0 || refused_for_now(rc)); i++)
        rc = flash_op_program_bytes(addr, (const uint8_t *)buf, len);
    flash_op_crumb(98);
    return rc;
}

static int fs_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    int rc = -1;
    flash_op_crumb(95);
    for (int i = 0; i < REFUSAL_TRIES && (i == 0 || refused_for_now(rc)); i++)
        rc = flash_op_erase(addr, FLASH_SECTOR_SIZE);
    flash_op_crumb(96);
    return rc;
}

const alfs_driver_t fs_driver = {
    .ctx = NULL,
    .read = fs_read,
    .prog = fs_prog,
    .erase = fs_erase,
};

static SemaphoreHandle_t fs_mutex;
static StaticSemaphore_t fs_mutex_buf;

#define FS_LOCK_WAIT_MS 10000u

void fs_lock_init(void) {
    if (!fs_mutex)
        fs_mutex = xSemaphoreCreateMutexStatic(&fs_mutex_buf);
}

bool fs_lock(void) {
    if (!rtos_running())
        return true;
    if (rtos_in_flight_task() || !fs_mutex)
        return false;
    return xSemaphoreTake(fs_mutex, pdMS_TO_TICKS(FS_LOCK_WAIT_MS)) == pdTRUE;
}

void fs_unlock(void) {
    if (rtos_running() && fs_mutex)
        xSemaphoreGive(fs_mutex);
}

bool fs_try_lock(void) {
    if (!rtos_running())
        return true;
    if (rtos_in_flight_task() || !fs_mutex)
        return false;
    return xSemaphoreTake(fs_mutex, 0) == pdTRUE;
}
