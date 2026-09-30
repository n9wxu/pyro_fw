/*
 * The high-rate log (src/sd/hr_log.c) on the host [DD-077]: its real reader
 * and writer, stepped by hand, against FatFs on a RAM disk. A fake IMU
 * numbers its sets in g[0], so a gap anywhere in a log is visible.
 */
#include "unity.h"
#include "hr_log.h"
#include "ff.h"
#include "diskio.h"
#include "device_status.h"
#include "pressure_trace.h"
#include "crc16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hr_log_test_power_cut(void);

/* ── The world the log sees ───────────────────────────────────────── */

static uint32_t now_us = 1000000u;
uint32_t time_us_32(void) {
    return now_us;
}
uint32_t hal_time_ms(void) {
    return now_us / 1000u;
}
static bool flying;
bool hal_log_active(void) {
    return flying;
}
volatile device_status_t g_status;
bool sd_mounted(void) {
    return true;
}
static uint32_t mounts = 1;
uint32_t sd_mount_count(void) {
    return mounts;
}

/* ── A 32 MB card ─────────────────────────────────────────────────── */

#define SECTORS 65536u
static uint8_t disk[SECTORS][512];
static uint32_t sectors_multi, sectors_single;
static uint32_t writes_to_fail; /* disk_write refuses while this counts down */
static uint32_t logging_multi, logging_single; /* while test HR_02 logged */

DSTATUS disk_status(BYTE p) {
    (void)p;
    return 0;
}
DSTATUS disk_initialize(BYTE p) {
    (void)p;
    return 0;
}
DRESULT disk_read(BYTE p, BYTE *b, LBA_t s, UINT n) {
    (void)p;
    memcpy(b, disk[s], (size_t)n * 512u);
    return RES_OK;
}
DRESULT disk_write(BYTE p, const BYTE *b, LBA_t s, UINT n) {
    (void)p;
    if (writes_to_fail) {
        writes_to_fail--;
        return RES_ERROR;
    }
    if (n >= 2)
        sectors_multi += n;
    else
        sectors_single += n;
    memcpy(disk[s], b, (size_t)n * 512u);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE p, BYTE cmd, void *buff) {
    (void)p;
    if (cmd == GET_SECTOR_COUNT)
        *(LBA_t *)buff = SECTORS;
    else if (cmd == GET_BLOCK_SIZE)
        *(DWORD *)buff = 1;
    return RES_OK;
}
int ff_mutex_create(int v) {
    (void)v;
    return 1;
}
void ff_mutex_delete(int v) {
    (void)v;
}
int ff_mutex_take(int v) {
    (void)v;
    return 1;
}
void ff_mutex_give(int v) {
    (void)v;
}

/* ── An IMU that numbers its sets ─────────────────────────────────── */

static uint16_t imu_seq;
static uint32_t imu_pending;

bool lsm6ds3_start(lsm6ds3_odr_t o) {
    (void)o;
    return true;
}
void lsm6ds3_stop(void) {}
uint32_t lsm6ds3_odr_hz(lsm6ds3_odr_t o) {
    return o == LSM6DS3_ODR_1660 ? 1660u : o == LSM6DS3_ODR_833 ? 833u : 104u;
}
bool lsm6ds3_read(lsm6ds3_set_t *out, uint32_t max, lsm6ds3_read_t *r) {
    uint32_t n = imu_pending < max ? imu_pending : max;
    for (uint32_t i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].g[0] = (int16_t)imu_seq++;
        out[i].a[2] = 2049;
    }
    imu_pending -= n;
    r->sets = n;
    r->overrun = false;
    r->backlog = 0;
    return true;
}

/* 10 ms of the world: the IMU's sets, a pressure and a temperature. */
static void world(void) {
    now_us += 10000u;
    imu_pending += 17u;
    ptrace_note(now_us - 3000u, now_us, 4000000u, 0, 10132500, PTRACE_PRESSURE);
    ptrace_note(now_us - 12000u, now_us, 8000000u, 0, 0, PTRACE_TEMPERATURE);
}

static void run(int periods, bool writer) {
    for (int i = 0; i < periods; i++) {
        world();
        hr_reader_step();
        if (writer)
            hr_writer_step();
    }
}

/* ── Reading a log back ───────────────────────────────────────────── */

typedef struct {
    bool header_ok;
    char reason[17];
    uint32_t odr;
    uint32_t imu_records, imu_sets, pres, flight;
    uint32_t seq_first, seq_last, gaps_inside, gaps_between;
    uint32_t bytes;
} decoded_t;

static uint8_t file_buf[4u * 1024u * 1024u];

static bool decode(const char *path, decoded_t *d) {
    memset(d, 0, sizeof(*d));
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    UINT got = 0;
    f_read(&f, file_buf, sizeof(file_buf), &got);
    f_close(&f);
    d->bytes = got;
    uint32_t o = 0;
    bool have_prev = false;
    uint16_t prev = 0;
    while (o + sizeof(hr_rec_t) <= got) {
        hr_rec_t h;
        memcpy(&h, file_buf + o, sizeof(h));
        if (h.type < HR_REC_HEADER || h.type > HR_REC_FLIGHT || o + sizeof(h) + h.len > got)
            break;
        const uint8_t *p = file_buf + o + sizeof(h);
        if (crc16(p, h.len) != h.crc)
            break; /* where a power cut ended the log */
        if (h.type == HR_REC_HEADER) {
            hr_header_t hh;
            memcpy(&hh, p, sizeof(hh));
            d->header_ok = o == 0 && memcmp(hh.magic, HR_MAGIC, 4) == 0 && hh.version == HR_VERSION;
            memcpy(d->reason, hh.reason, 16);
            d->odr = hh.odr_hz;
        } else if (h.type == HR_REC_IMU) {
            uint32_t n = (h.len - (uint32_t)sizeof(hr_imu_t)) / sizeof(lsm6ds3_set_t);
            d->imu_records++;
            for (uint32_t i = 0; i < n; i++) {
                lsm6ds3_set_t s;
                memcpy(&s, p + sizeof(hr_imu_t) + i * sizeof(s), sizeof(s));
                uint16_t q = (uint16_t)s.g[0];
                if (!have_prev)
                    d->seq_first = q;
                else if (q != (uint16_t)(prev + 1u)) {
                    *(i == 0 ? &d->gaps_between : &d->gaps_inside) += 1;
                    if (getenv("HR_DEBUG"))
                        printf("gap at off %u rec %u set %u: %u -> %u (n=%u)\n", (unsigned)o, (unsigned)d->imu_records,
                               (unsigned)i, (unsigned)prev, (unsigned)q, (unsigned)n);
                }
                prev = q;
                have_prev = true;
                d->imu_sets++;
            }
            d->seq_last = prev;
        } else if (h.type == HR_REC_PRES) {
            d->pres++;
        } else if (h.type == HR_REC_FLIGHT) {
            d->flight++;
        }
        o += sizeof(h) + h.len;
    }
    return true;
}

static bool exists(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

void setUp(void) {}
void tearDown(void) {}

/* ── Tests: one continuing story, in order ────────────────────────── */

void test_HR_01_between_flights_the_file_is_ready_and_the_ring_keeps_half(void) {
    hr_reader_begin();
    run(400, true);
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_TRUE(s.prepared);
    TEST_ASSERT_GREATER_THAN_UINT32(1024u * 1024u, s.expanded_bytes);
    TEST_ASSERT_FALSE(s.logging);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(32768u / 2u + 4096u, s.ring_used);
    TEST_ASSERT_TRUE(exists("/logs/next.bin"));
    TEST_ASSERT_FALSE(exists("/logs/hr0001.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, s.dropped_records);
}

void test_HR_02_a_flight_is_logged_whole_with_the_second_before_it(void) {
    sectors_multi = sectors_single = 0;
    flying = true;
    run(500, true); /* 5 s */
    uint32_t multi = sectors_multi, single = sectors_single;
    flying = false;
    run(2, true);
    logging_multi = multi;
    logging_single = single;
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_FALSE(s.logging);
    TEST_ASSERT_EQUAL_UINT32(1, s.logs);
    TEST_ASSERT_EQUAL_STRING("logs/hr0001.bin", s.file);
    decoded_t d;
    TEST_ASSERT_TRUE(decode("/logs/hr0001.bin", &d));
    /* Kept for support/hr_log.py, which the build decodes it with: the
     * firmware's encoder against the script's decoder. */
    const char *keep = getenv("HR_LOG_SAMPLE");
    if (keep) {
        FILE *fp = fopen(keep, "wb");
        if (fp) {
            fwrite(file_buf, 1, d.bytes, fp);
            fclose(fp);
        }
    }
    TEST_ASSERT_TRUE(d.header_ok);
    TEST_ASSERT_EQUAL_STRING("flight", d.reason);
    TEST_ASSERT_EQUAL_UINT32(1660, d.odr);
    TEST_ASSERT_EQUAL_UINT32(0, d.gaps_inside);
    TEST_ASSERT_EQUAL_UINT32(0, d.gaps_between);
    /* Every set of the flight, and more than half a second before it. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(500u * 17u + 850u, d.imu_sets);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1000u, d.pres);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(50u, d.flight);
    /* Truncated to what was written: nothing past the last record. */
    TEST_ASSERT_EQUAL_UINT32(s.file_bytes, d.bytes);
    TEST_ASSERT_TRUE(exists("/logs/next.bin")); /* the next is ready */
}

void test_HR_03_the_log_goes_to_the_card_in_multi_sector_writes(void) {
    /* An aligned 4 kB stage goes straight to the card, as many sectors a
     * call as a cluster allows, never a sector at a time through FatFs's
     * buffer. The single-sector writes are the directory and the FAT. */
    TEST_ASSERT_GREATER_THAN_UINT32(10u * logging_single, logging_multi);
}

void test_HR_04_a_log_a_power_cut_left_is_kept_under_a_number(void) {
    hr_log_start("bench");
    run(200, true);
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_TRUE(s.logging);
    hr_log_test_power_cut();
    hr_reader_begin();
    run(3, true); /* the writer prepares again: next.bin is found holding a log */
    TEST_ASSERT_TRUE(exists("/logs/hr0002.bin"));
    decoded_t d;
    TEST_ASSERT_TRUE(decode("/logs/hr0002.bin", &d));
    TEST_ASSERT_TRUE(d.header_ok);
    TEST_ASSERT_EQUAL_STRING("bench", d.reason);
    TEST_ASSERT_GREATER_THAN_UINT32(1000u, d.imu_sets);
    TEST_ASSERT_EQUAL_UINT32(0, d.gaps_inside);
}

void test_HR_05_a_full_ring_drops_whole_records_and_counts_them(void) {
    hr_stats_t before;
    hr_log_get_stats(&before);
    flying = true;
    run(1, true);    /* the log opens */
    run(400, false); /* 4 s with the writer stalled: 32 kB of ring overflows */
    run(300, true);  /* it catches up */
    flying = false;
    run(2, true);
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_GREATER_THAN_UINT32(before.dropped_records, s.dropped_records);
    decoded_t d;
    TEST_ASSERT_TRUE(decode("/logs/hr0003.bin", &d));
    TEST_ASSERT_TRUE(d.header_ok);
    TEST_ASSERT_EQUAL_UINT32(0, d.gaps_inside);         /* no record is torn  */
    TEST_ASSERT_GREATER_THAN_UINT32(0, d.gaps_between); /* whole ones dropped */
}

static FATFS fs;

/* [HR-06] A card mounted again under an open log -- POST /api/sd/init did it
 * on the bench -- leaves the log's file invalid, and every write to it fails.
 * The log goes on in a new file, from the next whole record. */
void test_HR_06_a_card_mounted_again_under_a_log_goes_on_in_a_new_file(void) {
    hr_stats_t before;
    hr_log_get_stats(&before);
    hr_log_start("bench");
    run(150, true);
    f_mount(NULL, "", 0);
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&fs, "", 1));
    mounts++;
    run(150, true);
    hr_log_stop();
    run(2, true);
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_EQUAL_UINT32(before.reopens + 1u, s.reopens);
    TEST_ASSERT_EQUAL_UINT32(before.write_errors, s.write_errors);
    decoded_t a, b;
    TEST_ASSERT_TRUE(decode("/logs/hr0004.bin", &a));
    TEST_ASSERT_TRUE(decode("/logs/hr0005.bin", &b));
    TEST_ASSERT_TRUE(a.header_ok && b.header_ok);
    TEST_ASSERT_EQUAL_STRING("bench", b.reason);
    TEST_ASSERT_GREATER_THAN_UINT32(1000u, a.imu_sets);
    TEST_ASSERT_GREATER_THAN_UINT32(1000u, b.imu_sets);
    TEST_ASSERT_EQUAL_UINT32(0, b.gaps_inside + b.gaps_between);
    uint16_t lost = (uint16_t)(b.seq_first - a.seq_last - 1u);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(17u, lost, "more than the torn record lost across the remount");
}

/* [HR-06] A card that refuses writes for a while: FatFs keeps a failed file's
 * error for good, so the log gives it up and, once the card takes writes
 * again, goes on in a new one. Lost: the one record the failed stage tore,
 * its head in the old file and its tail skipped. */
void test_HR_06_a_card_that_refuses_writes_for_a_while_loses_one_record(void) {
    hr_stats_t before;
    hr_log_get_stats(&before);
    hr_log_start("bench");
    run(100, true);
    writes_to_fail = 40;
    run(40, true);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, writes_to_fail, "the writer stopped trying");
    run(150, true);
    hr_log_stop();
    run(2, true);
    hr_stats_t s;
    hr_log_get_stats(&s);
    TEST_ASSERT_GREATER_THAN_UINT32(before.reopens, s.reopens);
    TEST_ASSERT_EQUAL_UINT32(before.dropped_records, s.dropped_records);
    decoded_t a, b;
    TEST_ASSERT_TRUE(decode("/logs/hr0006.bin", &a));
    TEST_ASSERT_TRUE(decode("/logs/hr0007.bin", &b));
    TEST_ASSERT_TRUE(a.header_ok && b.header_ok);
    TEST_ASSERT_EQUAL_UINT32(0, b.gaps_inside + b.gaps_between);
    uint16_t lost = (uint16_t)(b.seq_first - a.seq_last - 1u);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(17u, lost, "more than the torn record lost across the failure");
}

int main(void) {
    static BYTE work[4096];
    MKFS_PARM opt = {FM_ANY, 0, 0, 0, 0};
    if (f_mkfs("", &opt, work, sizeof(work)) != FR_OK || f_mount(&fs, "", 1) != FR_OK)
        return 2;
    UNITY_BEGIN();
    RUN_TEST(test_HR_01_between_flights_the_file_is_ready_and_the_ring_keeps_half);
    RUN_TEST(test_HR_02_a_flight_is_logged_whole_with_the_second_before_it);
    RUN_TEST(test_HR_03_the_log_goes_to_the_card_in_multi_sector_writes);
    RUN_TEST(test_HR_04_a_log_a_power_cut_left_is_kept_under_a_number);
    RUN_TEST(test_HR_05_a_full_ring_drops_whole_records_and_counts_them);
    RUN_TEST(test_HR_06_a_card_mounted_again_under_a_log_goes_on_in_a_new_file);
    RUN_TEST(test_HR_06_a_card_that_refuses_writes_for_a_while_loses_one_record);
    return UNITY_END();
}
