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
 * Verifies [DAT-02, DAT-03, DAT-06, DAT-07, DAT-10, WEB-API-06, FLT-LOG-06].
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
    .rate = LOG_RATE_1HZ,
};

static const char *const HDR_CSV = "# Pyro MK1C Flight Data\n# ID: PYRO001\n# Name: MyRocket\n"
                                   "# Pyro1: delay 0\n# Pyro2: agl 300\n# Units: m\n# Ground Pa: 101325\n"
                                   "# Log rate: 1 row/s\n"
                                   "time_ms,pressure_pa,altitude_cm,state,thrust,raw_pa,temp_c,event\n";

static void put(int n) {
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    bin_len += n;
}

static void header(const flog_header_t *h) {
    bin_len = 0;
    put(flog_put_header(bin, (int)sizeof(bin), h));
}

static void sample(uint32_t t, int32_t p, int32_t a, uint8_t st, uint8_t th, int32_t raw, int16_t temp, uint8_t ev) {
    flog_sample_t s = {t, p, a, raw, temp, st, th, ev};
    put(flog_put_sample(bin + bin_len, (int)sizeof(bin) - bin_len, &s));
}

/* A log with every kind of record, and the CSV it must render as. */
static void a_flight(char *want, size_t cap) {
    header(&HDR);
    size_t w = (size_t)snprintf(want, cap, "%s", HDR_CSV);
    for (int i = 0; i < 60; i++) {
        uint32_t t = (uint32_t)i * 1000u;
        uint8_t ev = i == 0 ? EVT_LAUNCH : i == 30 ? EVT_APOGEE : EVT_NONE;
        sample(t, 101325 - i * 97, i * 815 - 40, 4 + (i > 30), i < 3, 101320 - i * 96, (int16_t)(231 - i * 9), ev);
        w += (size_t)snprintf(want + w, cap - w, "%lu,%ld,%ld,%u,%u,%ld,%s%d.%d,%s\n", (unsigned long)t,
                              (long)(101325 - i * 97), (long)(i * 815 - 40), 4u + (i > 30), (unsigned)(i < 3),
                              (long)(101320 - i * 96), (231 - i * 9) < 0 ? "-" : "", abs(231 - i * 9) / 10,
                              abs(231 - i * 9) % 10, flight_event_name(ev));
        if (i % 20 == 5) {
            put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, t, FLOG_TAG_LUA, "servo,open\n", 11));
            w += (size_t)snprintf(want + w, cap - w, "%lu,,,,,,,LUA servo open \n", (unsigned long)t);
        }
    }
}

void setUp(void) {
    bin_len = 0;
}

void tearDown(void) {
}

void test_FLOG_01_the_header_renders_as_the_old_header_with_the_rate(void) {
    header(&HDR);
    render(bin_len, 4096, 0);
    TEST_ASSERT_EQUAL_STRING(HDR_CSV, csv);

    flog_header_t h = HDR;
    h.rate = LOG_RATE_FULL;
    h.units = 2;
    h.pyro1_mode = PYRO_MODE_SPEED;
    h.pyro1_value = 15;
    h.ground_pa = -3;
    header(&h);
    render(bin_len, 4096, 0);
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Pyro1: speed 15\n"));
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Units: ft\n# Ground Pa: -3\n# Log rate: every sample\n"));
    h.rate = LOG_RATE_EVENTS;
    header(&h);
    render(bin_len, 4096, 0);
    TEST_ASSERT_NOT_NULL(strstr(csv, "# Log rate: 1 row/s, every sample within 1 s of an event\n"));
}

void test_FLOG_02_samples_render_as_the_old_rows(void) {
    header(&HDR);
    sample(0, 101325, 0, 3, 0, 101330, 235, EVT_NONE);
    sample(4294967295u, INT32_MIN, -2147483647, 10, 1, INT32_MAX, -32768, EVT_APOGEE);
    sample(12, 1, -1, 4, 1, 0, -5, EVT_MAIN_FORCED);
    sample(13, 2, 2, 4, 0, 7, 0, EVT_LANDING);
    render(bin_len, 4096, 0);
    const char *rows = csv + strlen(HDR_CSV);
    TEST_ASSERT_EQUAL_STRING("0,101325,0,3,0,101330,23.5,\n"
                             "4294967295,-2147483648,-2147483647,10,1,2147483647,-3276.8,APOGEE\n"
                             "12,1,-1,4,1,0,-0.5,MAIN_FORCED\n"
                             "13,2,2,4,0,7,0.0,LANDING\n",
                             rows);
}

void test_FLOG_03_text_rows_are_cleaned_and_bounded(void) {
    header(&HDR);
    put(flog_put_text(bin + bin_len, (int)sizeof(bin) - bin_len, 77, FLOG_TAG_LUA, "a,b\tc\x7f" "d\xff", 8));
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_FLOG_01_the_header_renders_as_the_old_header_with_the_rate);
    RUN_TEST(test_FLOG_02_samples_render_as_the_old_rows);
    RUN_TEST(test_FLOG_03_text_rows_are_cleaned_and_bounded);
    RUN_TEST(test_FLOG_04_the_text_does_not_depend_on_how_it_is_read);
    RUN_TEST(test_FLOG_05_counting_gives_the_rendered_length);
    RUN_TEST(test_FLOG_06_a_log_cut_anywhere_renders_its_whole_records);
    RUN_TEST(test_FLOG_07_a_file_that_is_not_a_log_renders_nothing);
    RUN_TEST(test_FLOG_08_records_are_their_stated_size_and_never_overflow);
    return UNITY_END();
}
