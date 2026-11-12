/*
 * The flight log on disk: binary records, rendered as CSV when it is read
 * (DD-062). The CSV's header says every sample was kept, which the replay
 * checks [DAT-08].
 *
 * A file is the magic, one header record, then sample and text records in
 * the order they were written. A sample's time is signed: the log opens on
 * the pad's last second before T+0 [DAT-11]. Multi-byte fields are little-endian. A record
 * cut short by a power loss ends the log; everything before it renders.
 * Bytes that are no record -- erased flash, or what a dropped page left of
 * one -- render as one row naming where they start and how many there are,
 * and the records after them render [DAT-12].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_LOG_H
#define FLIGHT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define FLOG_PATH "flight_log.bin"
#define FLOG_MAGIC "PYL2"
#define FLOG_MAGIC_V1 "PYL1" /* read, never written: no estimator in its header */
#define FLOG_MAGIC_LEN 4

/* A sample record, type byte included: what the capacity estimate divides by. */
#define FLOG_SAMPLE_BYTES 22
#define FLOG_TEXT_MAX 64
#define FLOG_BOARD_MAX 32

enum { FLOG_TAG_LUA, FLOG_TAG_MOCK, FLOG_TAG_ESTIMATOR, FLOG_TAG_LOG };

typedef struct {
    const char *board;
    const char *id;
    const char *name; /* up to 8 characters each */
    uint8_t pyro1_mode;
    uint8_t pyro2_mode;
    uint16_t pyro1_value;
    uint16_t pyro2_value;
    uint8_t units;
    int32_t ground_pa;
    const char *estimator; /* up to 8 characters [SNS-EST-06] */
    bool model_decides;    /* [FLT-APO-09] */
} flog_header_t;

typedef struct {
    int32_t time_ms; /* from T+0: the pad's rows are before it [DAT-11] */
    int32_t pressure_pa;
    int32_t altitude_cm;
    int32_t raw_pa;
    int16_t temp_dc; /* tenths of a degree C */
    uint8_t state;
    uint8_t thrust;
    uint8_t event;
} flog_sample_t;

/* Each returns the bytes written, or 0 when they do not fit in cap. */
int flog_put_header(uint8_t *dst, int cap, const flog_header_t *h); /* the magic and the header record */
int flog_put_sample(uint8_t *dst, int cap, const flog_sample_t *s);
/* Commas and control characters become spaces; text past FLOG_TEXT_MAX is
 * dropped. */
int flog_put_text(uint8_t *dst, int cap, uint32_t time_ms, uint8_t tag, const char *text, int len);

/* The binary log's next bytes, up to n; fewer only at its end. */
typedef int (*flog_read_fn)(void *ctx, uint8_t *dst, int n);

#define FLOG_LINE_MAX 384
#define FLOG_RECORD_MAX (7 + FLOG_TEXT_MAX) /* a text record, the longest after the header */

typedef struct {
    flog_read_fn read;
    void *ctx;
    char line[FLOG_LINE_MAX];
    int line_len;
    int line_pos;
    bool started;
    bool ended;
    uint8_t ahead[FLOG_RECORD_MAX]; /* read, not yet rendered */
    int ahead_len;
    uint32_t at;     /* the file's offset of ahead[0] */
    int32_t last_ms; /* the last record's time, for a damage row's */
    uint32_t damaged_at;
    uint32_t skipped;
} flog_csv_t;

void flog_csv_init(flog_csv_t *r, flog_read_fn read, void *ctx);

/* The next CSV bytes, up to max, into dst; with dst NULL, only counted.
 * Returns 0 at the end. A file that is not a flight log renders nothing. */
int flog_csv_read(flog_csv_t *r, char *dst, int max);

#endif
