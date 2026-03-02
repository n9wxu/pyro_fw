/*
 * fat_mimic - FAT12 USB Mass Storage over littlefs
 *
 * Presents a writable FAT12 filesystem to USB hosts while persisting
 * all changes to littlefs on flash. No flash I/O in USB callbacks.
 */
#ifndef FAT_MIMIC_H
#define FAT_MIMIC_H

#include <lfs.h>
#include <stdint.h>
#include <stdbool.h>

#define FAT_MIMIC_SECTOR_SIZE 512

void fat_mimic_init(const struct lfs_config *cfg);
void fat_mimic_mount(void);
void fat_mimic_unmount(void);
uint32_t fat_mimic_capacity(void);
void fat_mimic_read(uint8_t lun, uint32_t lba, void *buf, uint32_t len);
void fat_mimic_write(uint8_t lun, uint32_t lba, void *buf, uint32_t len);
void fat_mimic_poll(void);

#endif
