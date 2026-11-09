/*
 * The flight log's binary records, and the CSV they render as.
 *
 * The CSV is held to the exact text the log was stored as before DD-062, so
 * the web UI, the replay and any spreadsheet read it as they always have.
 * Rendering is a stream, like the HTTP response it feeds: it must not depend
 * on how the reads are divided, and a log cut short by a power loss renders
 * every whole record before the cut.
 *
 * SPDX-License-Identifier: MIT
 *
 * Verifies [DAT-02, DAT-03, DAT-06, DAT-07, DAT-11, DAT-10, DAT-12, WEB-API-06, FLT-LOG-06].
 */
#include "unity.h"
#include "../src/config.h"
#include "../src/flight_events.h"
#include "../src/flight_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t bin[16384];
static int bin_len;
static char csv[65536];

typedef struct {
    const uint8_t *p;
    int len, pos, most; /* most: the largest read it will return */
} src_t;

static int src_read(void *ctx, uint8_t *dst, int n) {
    src_t *s = (src_t *)ctx;
    if (s->most > 0 && n > s->most) {
        n = s->most;
    }
    int left = s->len - s->pos;
    if (n > left) {
        n = left;
    }
    memcpy(dst, s->p + s->pos, (size_t)n);
    s->pos += n;
    return n;
}

/* The whole CSV of bin[0..len), read out in pieces of `piece` bytes. */
static int render(int len, int piece, int most) {
    src_t s = {bin, len, 0, most};
    flog_csv_t r;
    flog_csv_init(&r, src_read, &s);
    int n = 0, k;
    while ((k = flog_csv_read(&r, csv + n, piece)) > 0) {
        n += k;
        TEST_ASSERT_TRUE(n < (int)sizeof(csv));
    }
    csv[n] = '\0';
    return n;
}

static const flog_header_t HDR = {
    .board = "Pyro MK1C",
    .id = "PYRO001",
    .name = "MyRocket",
    .pyro1_mode = PYRO_MODE_DELAY,
    .pyro1_value = 0,
    .pyro2_mode = PYRO_MODE_AGL,
    .pyro2_value = 300,
    .units = 1,
    .ground_pa = 101325,
    .estimator = "constacc",
    .model_decides = true,
};

#define HDR_CSV_V1                                                                                                     \
    "# Pyro MK1C Flight Data\n# ID: PYRO001\n# Name: MyRocket\n"                                                       \
    "# Pyro1: delay 0\n# Pyro2: agl 300\n# Units: m\n# Ground Pa: 101325\n"                                            \
    "# Log rate: every sample\n"
#define COLUMNS "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event\n"

static const char *const HDR_CSV = HDR_CSV_V1 "# Estimator: constacc\n# Apogee decided by: estimator\n" COLUMNS;

static void put(int n) {
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    bin_len += n;
}

static void header(const flog_header_t *h) {
    bin_len = 0;
    put(flog_put_header(bin, (int)sizeof(bin), h));
}

static void sample(int32_t t, int32_t p, int32_t a, uint8_t st, uint8_t th, int32_t raw, int16_t temp, uint8_t ev) {
    flog_sample_t s = {t, p, a, raw, temp, st, th, ev};
    put(flog_put_sample(bin + bin_len, (int)sizeof(bin) - bin_len, &s));
}

/* A log with every kind of record, and the CSV it must render as. */
static void a_flight(char *want, size_t cap) {
    header(&HDR);
    size_t w = (size_t)snprintf(want, cap, "%s", HDR_CSV);
    for (int i = 0; i < 60; i++) {
        int32_t t = i * 1000;
        uint8_t ev = i == 0 ? EVT_LAUNCH : i == 30 ? EVT_APOGEE : EVT_NONE;
        sample(t, 101325 - i * 97, i * 815 - 40, 4 + (i > 30), i < 3, 101320 - i * 96, (int16_t)(231 - i * 9), ev);
        w += (size_t)snprintf(want + w, cap - w, "%ld,%ld,%ld,%u,%u,%ld,%s%d.%d,%s\n", (long)t, (long)(101325 - i * 97),
                              (long)(i * 815 - 40), 4u + (i > 30), (unsigned)(i < 3), (long)(101320 - i * 96),
                              (231 - i * 9) < 0 ? "-" : "", abs(231 - i * 9) / 10, abs(231 - i * 9) % 10,
                              flight_event_name(ev));
        if (i % 20 == 5) {
            put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, (uint32_t)t, FLOG_TAG_LUA, "servo,open\n",
                              11));
            w += (size_t)snprintf(want + w, cap - w, "%ld,,,,,,,LUA servo open \n", (long)t);
        }
    }
}

void setUp(void) {
    bin_len = 0;
}

void tearDown(void) {}

/* [FLT-LOG-07, DAT-08] The header's plan byte says every sample is kept, and
 * so does the CSV the replay reads. */
void test_FLOG_01_the_header_renders_with_every_sample_kept(void) {
    header(&HDR);
    TEST_ASSERT_EQUAL_UINT8(2, bin[FLOG_MAGIC_LEN + 29]);
    render(bin_len, 4096, 0);
    TEST_ASSERT_EQUAL_STRING(HDR_CSV, csv);

    flog_header_t h = HDR;
    h.units = 2;
    h.pyro1_mode = PYRO_MODE_SPEED;
    h.pyro1_value = 15;
    h.ground_pa = -3;
    header(&h);
    render(bin_len, 4096, 0);
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Pyro1: speed 15\n"));
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Units: ft\n# Ground Pa: -3\n# Log rate: every sample\n"));
}

void test_FLOG_02_samples_render_as_the_old_rows(void) {
    header(&HDR);
    sample(0, 101325, 0, 3, 0, 101330, 235, EVT_NONE);
    sample(INT32_MIN, INT32_MIN, -2147483647, 10, 1, INT32_MAX, -32768, EVT_APOGEE);
    sample(12, 1, -1, 4, 1, 0, -5, EVT_MAIN_FORCED);
    sample(13, 2, 2, 4, 0, 7, 0, EVT_LANDING);
    render(bin_len, 4096, 0);
    const char *rows = csv + strlen(HDR_CSV);
    TEST_ASSERT_EQUAL_STRING("0,101325,0,3,0,101330,23.5,\n"
                             "-2147483648,-2147483648,-2147483647,10,1,2147483647,-3276.8,APOGEE\n"
                             "12,1,-1,4,1,0,-0.5,MAIN_FORCED\n"
                             "13,2,2,4,0,7,0.0,LANDING\n",
                             rows);
}

void test_FLOG_03_text_rows_are_cleaned_and_bounded(void) {
    header(&HDR);
    put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, 77, FLOG_TAG_LUA,
                      "a,b\tc\x7f"
                      "d\xff",
                      8));
    put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, 78, FLOG_TAG_MOCK, "pyro1 fire", 10));
    char longtext[200];
    memset(longtext, 'x', sizeof(longtext));
    put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, 79, FLOG_TAG_LUA, longtext, (int)sizeof(longtext)));
    render(bin_len, 4096, 0);
    const char *rows = csv + strlen(HDR_CSV);
    char want[256];
    int w = snprintf(want, sizeof(want), "77,,,,,,,LUA a b c d \n78,,,,,,,MOCK pyro1 fire\n79,,,,,,,LUA ");
    memset(want + w, 'x', FLOG_TEXT_MAX);
    strcpy(want + w + FLOG_TEXT_MAX, "\n");
    TEST_ASSERT_EQUAL_STRING(want, rows);
}

void test_FLOG_04_the_text_does_not_depend_on_how_it_is_read(void) {
    static char want[65536], whole[65536];
    a_flight(want, sizeof(want));
    int n = render(bin_len, 65536, 0);
    TEST_ASSERT_EQUAL_STRING(want, csv);
    memcpy(whole, csv, (size_t)n + 1);
    for (int piece = 1; piece <= 97; piece++) {
        render(bin_len, piece, 0);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(whole, csv, "CSV read in small pieces");
    }
    for (int most = 1; most <= 23; most++) {
        render(bin_len, 700, most);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(whole, csv, "binary read in short pieces");
    }
}

/* The download's Content-Length is counted before a byte is sent. */
void test_FLOG_05_counting_gives_the_rendered_length(void) {
    static char want[65536];
    a_flight(want, sizeof(want));
    int n = render(bin_len, 4096, 0);
    src_t s = {bin, bin_len, 0, 0};
    flog_csv_t r;
    flog_csv_init(&r, src_read, &s);
    int counted = 0, k;
    while ((k = flog_csv_read(&r, NULL, 333)) > 0) {
        counted += k;
    }
    TEST_ASSERT_EQUAL_INT(n, counted);
}

/* A power loss can cut the file anywhere: every whole record renders, and
 * nothing of the cut one. */
void test_FLOG_06_a_log_cut_anywhere_renders_its_whole_records(void) {
    static char want[65536], whole[65536];
    a_flight(want, sizeof(want));
    render(bin_len, 4096, 0);
    strcpy(whole, csv);
    int full = bin_len;
    for (int cut = 0; cut < full; cut++) {
        int n = render(cut, 509, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(whole, csv, (size_t)n), "a prefix of the whole log");
        TEST_ASSERT_TRUE_MESSAGE(n == 0 || csv[n - 1] == '\n', "whole lines only");
    }
}

void test_FLOG_07_a_file_that_is_not_a_log_renders_nothing(void) {
    const char *old = "# Pyro MK1C Flight Data\ntime_ms,pressure_pa\n1,2\n";
    memcpy(bin, old, strlen(old));
    TEST_ASSERT_EQUAL_INT(0, render((int)strlen(old), 4096, 0));
    TEST_ASSERT_EQUAL_INT(0, render(0, 4096, 0));
}

void test_FLOG_08_records_are_their_stated_size_and_never_overflow(void) {
    flog_sample_t s = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t buf[64];
    TEST_ASSERT_EQUAL_INT(FLOG_SAMPLE_BYTES, flog_put_sample(buf, (int)sizeof(buf), &s));
    TEST_ASSERT_EQUAL_INT(0, flog_put_sample(buf, FLOG_SAMPLE_BYTES - 1, &s));
    TEST_ASSERT_EQUAL_INT(0, flog_put_text(buf, 8, 1, FLOG_TAG_LUA, "hello world", 11));
    TEST_ASSERT_EQUAL_INT(0, flog_put_header(buf, 8, &HDR));
    flog_header_t h = HDR;
    h.board = "a board name longer than the thirty-two characters kept";
    uint8_t big[256];
    TEST_ASSERT_GREATER_THAN_INT(0, flog_put_header(big, (int)sizeof(big), &h));
}

/* [DAT-11] The pad's rows come before T+0, so a time is signed. */
void test_DAT_11_a_row_before_t0_renders_its_negative_time(void) {
    header(&HDR);
    sample(-1000, 101325, 0, 3, 0, 101326, 215, EVT_NONE);
    sample(-20, 101324, 8, 3, 0, 101300, 215, EVT_NONE);
    sample(0, 101310, 120, 4, 1, 101250, 215, EVT_LAUNCH);
    render(bin_len, 4096, 0);
    TEST_ASSERT_EQUAL_STRING("-1000,101325,0,3,0,101326,21.5,\n-20,101324,8,3,0,101300,21.5,\n"
                             "0,101310,120,4,1,101250,21.5,LAUNCH\n",
                             csv + strlen(HDR_CSV));
}

/* [DAT-08] The header names the estimator and which filter decided, for the
 * replay. */
void test_DAT_08_the_header_names_the_estimator_and_the_deciding_filter(void) {
    flog_header_t h = HDR;
    h.estimator = "constacc";
    h.model_decides = false;
    header(&h);
    render(bin_len, 4096, 0);
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Estimator: constacc\n# Apogee decided by: simple filter\n" COLUMNS));
}

/* A log written before the estimator was in the header still renders. */
void test_DAT_08_a_version_1_log_renders_as_it_did(void) {
    header(&HDR);
    /* Version 1: no estimator[8] or model_decides between the plan and the board. */
    static uint8_t v1[sizeof(bin)];
    memcpy(v1, "PYL1", FLOG_MAGIC_LEN);
    int rec = FLOG_MAGIC_LEN;
    memcpy(v1 + rec, bin + rec, 30);
    v1[rec + 1] = (uint8_t)(bin[rec + 1] - 9);
    int board = bin_len - rec - 39;
    memcpy(v1 + rec + 30, bin + rec + 39, (size_t)board);
    int v1_len = rec + 30 + board;
    flog_sample_t s = {0, 101325, 0, 101330, 235, 4, 1, EVT_LAUNCH};
    v1_len += flog_put_sample(v1 + v1_len, (int)sizeof(v1) - v1_len, &s);
    memcpy(bin, v1, (size_t)v1_len);
    render(v1_len, 4096, 0);
    TEST_ASSERT_EQUAL_STRING(HDR_CSV_V1 COLUMNS "0,101325,0,4,1,101330,23.5,LAUNCH\n", csv);
}

/* ── A damaged log [DAT-12] ──────────────────────────────────────── */

/* Each record's offset in bin, from a_flight(): the header's end first. */
static int record_at[256];
static int records;

static void index_records(void) {
    records = 0;
    int i = FLOG_MAGIC_LEN + 2 + bin[FLOG_MAGIC_LEN + 1];
    while (i < bin_len) {
        record_at[records++] = i;
        i += bin[i] == 'S' ? FLOG_SAMPLE_BYTES : 7 + bin[i + 6];
    }
    record_at[records] = bin_len;
}

/* The CSV's lines from the n-th record's to the m-th's, exclusive. */
static const char *lines_of(const char *whole, int n, int m, char *out, size_t cap) {
    const char *p = whole + strlen(HDR_CSV);
    for (int i = 0; i < n; i++)
        p = strchr(p, '\n') + 1;
    const char *q = p;
    for (int i = n; i < m; i++)
        q = strchr(q, '\n') + 1;
    size_t len = (size_t)(q - p);
    TEST_ASSERT_TRUE(len < cap);
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/* The time column of the n-th record's line. */
static long time_of(const char *whole, int n) {
    char line[512];
    lines_of(whole, n, n + 1, line, sizeof(line));
    return strtol(line, NULL, 10);
}

/* A page of erased flash where records should be: every record renders, and
 * a row says where the log broke and how much was passed over. */
void test_DAT_12_erased_bytes_inside_a_log_are_named_and_passed_over(void) {
    static char want[65536], whole[65536], before[65536], after[65536];
    a_flight(want, sizeof(want));
    render(bin_len, 4096, 0);
    strcpy(whole, csv);
    index_records();
    int k = records / 2, at = record_at[k];
    memmove(bin + at + 256, bin + at, (size_t)(bin_len - at));
    memset(bin + at, 0xff, 256);
    bin_len += 256;
    render(bin_len, 4096, 0);
    char expect[65536];
    snprintf(expect, sizeof(expect), "%s%s%ld,,,,,,,LOG DAMAGED at byte %d: 256 bytes skipped\n%s", HDR_CSV,
             lines_of(whole, 0, k, before, sizeof(before)), time_of(whole, k - 1), at,
             lines_of(whole, k, records, after, sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(expect, csv);
}

/* A page's bytes dropped from the middle of a record [L4]: the stream goes on
 * mid-record. The broken record's remains are passed over, and the records
 * after it render. */
void test_DAT_12_a_record_cut_mid_log_is_passed_over_and_the_log_goes_on(void) {
    static char want[65536], whole[65536], before[65536], after[65536];
    a_flight(want, sizeof(want));
    render(bin_len, 4096, 0);
    strcpy(whole, csv);
    index_records();
    int k = records / 3;
    TEST_ASSERT_EQUAL_CHAR('S', bin[record_at[k]]);
    int cut = record_at[k] + 1, gone = 9; /* the type byte goes, and 9 of the record's bytes */
    memmove(bin + record_at[k], bin + cut + gone, (size_t)(bin_len - cut - gone));
    bin_len -= 1 + gone;
    int left = FLOG_SAMPLE_BYTES - 1 - gone;
    render(bin_len, 509, 0);
    char expect[65536];
    snprintf(expect, sizeof(expect), "%s%s%ld,,,,,,,LOG DAMAGED at byte %d: %d bytes skipped\n%s", HDR_CSV,
             lines_of(whole, 0, k, before, sizeof(before)), time_of(whole, k - 1), record_at[k], left,
             lines_of(whole, k + 1, records, after, sizeof(after)));
    TEST_ASSERT_EQUAL_STRING(expect, csv);
}

/* Bytes that are no record run to the end of the file: the row says so, and
 * nothing of them renders. */
void test_DAT_12_damage_to_the_end_of_the_file_is_named(void) {
    static char want[65536], whole[65536];
    a_flight(want, sizeof(want));
    render(bin_len, 4096, 0);
    strcpy(whole, csv);
    index_records();
    int at = bin_len;
    memset(bin + bin_len, 0xff, 300);
    bin_len += 300;
    render(bin_len, 4096, 0);
    char expect[65536];
    snprintf(expect, sizeof(expect), "%s%ld,,,,,,,LOG DAMAGED at byte %d: 300 bytes skipped\n", whole,
             time_of(whole, records - 1), at);
    TEST_ASSERT_EQUAL_STRING(expect, csv);
}

/* The rows the board writes about itself carry their own tag. */
void test_DAT_12_the_log_s_own_rows_render_as_LOG(void) {
    header(&HDR);
    put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, 4623, FLOG_TAG_LOG, "OUT OF ROOM", 11));
    render(bin_len, 4096, 0);
    TEST_ASSERT_EQUAL_STRING("4623,,,,,,,LOG OUT OF ROOM\n", csv + strlen(HDR_CSV));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLOG_01_the_header_renders_with_every_sample_kept);
    RUN_TEST(test_FLOG_02_samples_render_as_the_old_rows);
    RUN_TEST(test_FLOG_03_text_rows_are_cleaned_and_bounded);
    RUN_TEST(test_FLOG_04_the_text_does_not_depend_on_how_it_is_read);
    RUN_TEST(test_FLOG_05_counting_gives_the_rendered_length);
    RUN_TEST(test_FLOG_06_a_log_cut_anywhere_renders_its_whole_records);
    RUN_TEST(test_FLOG_07_a_file_that_is_not_a_log_renders_nothing);
    RUN_TEST(test_FLOG_08_records_are_their_stated_size_and_never_overflow);
    RUN_TEST(test_DAT_11_a_row_before_t0_renders_its_negative_time);
    RUN_TEST(test_DAT_08_the_header_names_the_estimator_and_the_deciding_filter);
    RUN_TEST(test_DAT_08_a_version_1_log_renders_as_it_did);
    RUN_TEST(test_DAT_12_erased_bytes_inside_a_log_are_named_and_passed_over);
    RUN_TEST(test_DAT_12_a_record_cut_mid_log_is_passed_over_and_the_log_goes_on);
    RUN_TEST(test_DAT_12_damage_to_the_end_of_the_file_is_named);
    RUN_TEST(test_DAT_12_the_log_s_own_rows_render_as_LOG);
    return UNITY_END();
}
