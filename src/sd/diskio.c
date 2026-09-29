/*
 * FatFs's disk layer: one drive, the SD card (sd_card.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include "ff.h"
#include "diskio.h"
#include "sd_card.h"

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && sd_type() != SD_NONE) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0)
        return STA_NOINIT;
    if (sd_type() == SD_NONE)
        sd_init_card();
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || sd_type() == SD_NONE)
        return RES_NOTRDY;
    return sd_read(buff, (uint32_t)sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || sd_type() == SD_NONE)
        return RES_NOTRDY;
    return sd_write(buff, (uint32_t)sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || sd_type() == SD_NONE)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return sd_sync() == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sd_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SD_SECTOR;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1; /* not known: FatFs aligns nothing to it */
        return RES_OK;
    case CTRL_TRIM:
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
