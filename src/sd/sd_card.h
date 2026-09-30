/*
 * An SD card in SPI mode [DD-075], from the SD Association's Physical Layer
 * Simplified Specification 6.00 (docs/datasheets/, PDF pages; the printed
 * number is 18 less).
 *
 * CRC is on for commands and data (CMD59), as the specification recommends
 * before ACMD41 (PDF page 230): on a wire-wrapped bus a corrupted byte fails
 * as an error and a retry, never as a wrong sector.
 *
 * Every wait is bounded by the specification's own limits -- reads 100 ms,
 * write busy 250 ms and up to 500 ms on SDXC (PDF page 97) -- and a busy wait
 * longer than a millisecond gives the bus back between polls, so the
 * accelerometer is read while the card programs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include <stdint.h>

#define SD_SECTOR 512u

typedef enum { SD_NONE = 0, SD_V1, SD_V2_SC, SD_V2_HC } sd_type_t;

/* Brings a card up and mounts its FAT, once, at boot. 0, or <0 with no card
 * or one that will not answer; the board then keeps every file in littlefs. */
int sd_start(void);

/* True while a card is mounted and taking files. */
bool sd_mounted(void);

/* Mounts since boot. A file opened under one mount is invalid under the
 * next: FatFs refuses every operation on it. */
uint32_t sd_mount_count(void);

/* The card layer, for FatFs's diskio.c. 0, or <0. */
int sd_init_card(void);
int sd_read(uint8_t *buf, uint32_t lba, uint32_t count);
int sd_write(const uint8_t *buf, uint32_t lba, uint32_t count);
int sd_sync(void);
uint32_t sd_sectors(void);
sd_type_t sd_type(void);

/* Counters for /api/sd. */
typedef struct {
    uint32_t hz;            /* the data clock in use */
    uint32_t reads, writes; /* commands */
    uint32_t sectors_read, sectors_written;
    uint32_t crc_errors; /* a data block whose CRC16 did not match */
    uint32_t cmd_errors; /* a command the card refused or did not answer */
    uint32_t timeouts;
    uint32_t retries;
    uint32_t busy_max_us;  /* the longest the card held its busy line */
    uint32_t write_max_us; /* the longest one sd_write() took */
    uint32_t mount_rc;     /* FatFs's FRESULT from the mount */
    uint8_t init_r1[6];    /* CMD0, CMD8, CMD59, ACMD41, CMD58, CMD9: the last init's R1s */
    uint8_t init_r7[4];    /* CMD8's echo */
    uint8_t init_ocr[4];
    uint8_t cmd55_first, acmd41_first;
    uint32_t acmd41_polls, acmd41_ones;
    uint32_t acmd41_other_ms; /* when a poll first answered neither 0 nor 1 */
    uint8_t acmd41_other;
    /* After that answer: CMD58, which a card still in SPI mode answers, then
     * CMD0, which one that has reset to SD mode answers too. Each CMD0 that
     * answers idle restarts the initialisation, up to SD_INIT_RESTARTS. */
    uint8_t after_r58, after_r0;
    uint8_t restarts;
    uint16_t fail_ms[4];   /* since the first ACMD41, each attempt's end */
    uint32_t init_yields; /* bus given away during an initialisation */
} sd_stats_t;

void sd_set_init_timeout_ms(uint32_t ms);
void sd_set_poll_gap_ms(uint32_t ms);
/* How many times a card that resets inside ACMD41 is brought up again in one
 * sd_start(), within its timeout: many, to hold a failing card's current
 * draw on the rail long enough to measure. */
void sd_set_init_restarts(uint32_t n);

/* The data clock the next initialisation sets, BOARD_SD_SPI_HZ by default;
 * the peripheral rounds it down to one it can make. */
void sd_set_data_hz(uint32_t hz);

/* The initialisation's clock, with the card deselected, for ms: the bus's
 * own disturbance without the card's draw, for the bench. */
bool sd_clock_idle(uint32_t ms);

/* CMD59 at the next init: on by default. */
void sd_set_crc(bool on);

/* The LSM6DS3's WHO_AM_I (0Fh), 0x69 (LSM6DS3_DocID026899_Rev4_2015-04.pdf,
 * page 51): a check that the shared bus works at all. */
uint8_t sd_bus_probe_imu(void);

void sd_get_stats(sd_stats_t *out);

/* The card's CID and OCR as hex, for /api/sd. */
const char *sd_cid(void);

#endif
