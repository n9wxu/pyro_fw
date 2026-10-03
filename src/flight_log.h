/*
 * The flight log on disk: binary records, rendered as CSV when it is read
 * (DD-062). The CSV is the text the log used to be stored as, plus a line
 * saying the rate it was written at.
 *
 * A file is the magic, one header record, then sample and text records in
 * the order they were written. Multi-byte fields are little-endian. A record
 * cut short by a power loss ends the log; everything before it renders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLIGHT_LOG_H
#define FLIGHT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define FLOG_PATH "flight_log.bin"
#define FLOG_MAGIC "PYL1"
#define FLOG_MAGIC_LEN 4

/* A sample record, type byte included: what the capacity estimate divides by. */
#define FLOG_SAMPLE_BYTES 22
#define FLOG_TEXT_MAX 64
#define FLOG_BOARD_MAX 32

enum { FLOG_TAG_LUA, FLOG_TAG_MOCK, FLOG_TAG_ESTIMATOR };

typedef struct {
    const char *board;
    const char *id, *name; /* up to 8 characters each */
    uint8_t pyro1_mode, pyro2_mode;
    uint16_t pyro1_value, pyro2_value;
    uint8_t units;
    int32_t ground_pa;
    uint8_t rate; /* log_rate_t */
} flog_header_t;

typedef struct {
    uint32_t time_ms;
    int32_t pressure_pa, altitude_cm, raw_pa;
    int16_t temp_dc; /* tenths of a degree C */
    uint8_t state, thrust, event;
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

typedef struct {
    flog_read_fn read;
    void *ctx;
    char line[FLOG_LINE_MAX];
    int line_len, line_pos;
    bool started, ended;
} flog_csv_t;

void flog_csv_init(flog_csv_t *r, flog_read_fn read, void *ctx);

/* The next CSV bytes, up to max, into dst; with dst NULL, only counted.
 * Returns 0 at the end. A file that is not a flight log renders nothing. */
int flog_csv_read(flog_csv_t *r, char *dst, int max);

#endif
