/*
 * The high-rate log, on the SD card [DD-077].
 *
 * Everything the board senses, at the rate it senses it: every LSM6DS3 FIFO
 * set, every pressure and temperature conversion (pressure_trace.h), and a
 * snapshot of the flight a tenth of a second. Two tasks on core1 at P:
 *
 *   reader  every 10 ms drains the IMU's FIFO, takes the new conversions,
 *           and appends records to a lock-free ring (32 kB, ~1.6 s at
 *           1.66 kHz);
 *   writer  empties the ring onto the card in 4 kB chunks that keep the file
 *           sector-aligned, so FatFs writes them straight through.
 *
 * The flight task is untouched: nothing here is on its path.
 *
 * Logging runs while the flight log does -- launch to landing -- or from a
 * POST /api/hr/start on the bench. Between flights the ring keeps its newest
 * half, so a log opens with the second before launch. The writer creates and
 * preallocates the next file (f_expand, contiguous) on the pad, so launch
 * waits on no FAT search; it is logs/next.bin until it closes, then renamed
 * logs/hrNNNN.bin. One left behind by a power cut is renamed at the next
 * boot. A file the card cannot take any more -- mounted again under it, or
 * writes that keep failing -- is given up, and the log goes on in a new one
 * from the next whole record [HR-06].
 *
 * The file is records, little-endian: a u8 type, a u8 of flags, a u16 payload
 * length, a u16 CRC of the payload, then the payload. The first is the
 * header, padded to 4 kB. A record whose CRC does not match is where the
 * log ends: a power cut can leave the last one's payload unwritten, and the
 * preallocated space past it holds whatever the card held. support/hr_log.py
 * decodes them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HR_LOG_H
#define HR_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include "lsm6ds3.h"

#define HR_MAGIC "PYHR"
#define HR_VERSION 2

enum {
    HR_REC_HEADER = 1, /* hr_header_t, padded to 4096 bytes in all      */
    HR_REC_IMU = 2,    /* hr_imu_t, then its sets (lsm6ds3_set_t)       */
    HR_REC_PRES = 3,   /* ptrace_rec_t: one conversion, as traced       */
    HR_REC_FLIGHT = 4, /* hr_flight_t                                   */
};

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t len; /* payload bytes */
    uint16_t crc; /* CRC-16/XMODEM of the payload (crc16.h) */
} hr_rec_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version;
    uint16_t header_len;
    uint32_t odr_hz;
    uint32_t ug_per_lsb;   /* accelerometer scale */
    uint32_t mdps_per_lsb; /* gyroscope scale     */
    uint32_t open_us;      /* the timer when the file opened */
    uint32_t open_ms;
    char board[16];
    char reason[16];
} hr_header_t;

/* The sets follow: the newest was in the FIFO backlog_words words before the
 * end when the FIFO status was read, at read_us -- after the bus was had,
 * which the card may hold for a stage -- so set i of n is at
 *   read_us - (backlog_words / 6 + n - 1 - i) / rate,
 * the rate the sensor's own, measured from the log (support/hr_log.py). */
typedef struct __attribute__((packed)) {
    uint32_t read_us;
    uint16_t backlog_words;
    uint8_t overrun; /* the FIFO filled and dropped sets before this read */
    uint8_t pad;
} hr_imu_t;

typedef struct __attribute__((packed)) {
    uint32_t t_us;
    uint8_t state;
    uint8_t thrust;
    uint16_t pad;
    int32_t alt_cm;
    int32_t speed_cms;
    int32_t pressure_pa;
} hr_flight_t;

typedef struct {
    bool logging, preparing, prepared, imu_ok, card;
    uint32_t odr_hz;
    char file[20];
    uint32_t file_bytes;     /* this file so far */
    uint32_t expanded_bytes; /* preallocated */
    uint32_t ring_used, ring_max;
    uint32_t dropped_records, dropped_bytes; /* the ring was full */
    uint32_t imu_sets, imu_reads, imu_overruns, imu_backlog_max, imu_read_fails;
    uint32_t pres_records, flight_records;
    uint32_t writes, write_max_us, write_errors;
    uint32_t syncs, sync_max_us;
    uint32_t prepare_us;  /* the last create and f_expand */
    uint32_t logs;        /* files closed since boot */
    uint32_t reopens;     /* files given up: the card remounted under one, or
                             WRITE_FAILS_MAX writes in a row failed */
    uint32_t bytes_total; /* written since boot */
    lsm6ds3_set_t last;   /* the newest set read */
} hr_stats_t;

/* Before the scheduler: the two tasks. */
/* The state a start finds: nothing open, nothing queued. A log a power cut
 * left on the card is found and kept by the writer [HR-04]. */
void hr_log_init(void);
void hr_log_create_tasks(void);

/* One pass of each task's loop: the reader's every 10 ms, the writer's
 * whenever the reader wakes it. */
void hr_reader_begin(void);
void hr_reader_step(void);
void hr_writer_step(void);

/* The bench's start and stop; a flight starts and stops the log itself. */
bool hr_log_start(const char *reason);
void hr_log_stop(void);

/* The IMU's rate, from the next log. */
bool hr_log_set_odr(lsm6ds3_odr_t odr);

void hr_log_get_stats(hr_stats_t *out);

#endif
