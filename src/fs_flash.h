/*
 * The filesystem area on the boot flash, as alfs reaches it [DD-100].
 *
 * The top PYRO_FS_SIZE_KB of the chip. Reads come through the XIP no-cache,
 * no-allocate alias (RP2040 datasheet 2.6.3.1), so they never see a cache
 * line a program left stale. Every program and erase runs under flash_op()'s
 * lockout; a program writes exactly the bytes alfs gives it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FS_FLASH_H
#define FS_FLASH_H

#include "alfs.h"

extern const alfs_area_t fs_area;
extern const alfs_driver_t fs_driver;

#endif
