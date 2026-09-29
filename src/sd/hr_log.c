/*
 * The high-rate log. See hr_log.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hr_log.h"
#include "sd_card.h"
#include "crc16.h"
#include "pressure_trace.h"
#include "device_status.h"
#include "board_id.h"
#include "hal.h"
#include "rtos_tasks.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include <stdio.h>
#include <string.h>

#define RING_SIZE 32768u /* a power of two */
#define RING_MASK (RING_SIZE - 1u)
#define STAGE 4096u      /* one write: eight sectors, keeping the file aligned */
#define READ_MS 10u      /* the reader's period */
#define FLIGHT_EVERY 10u /* a flight snapshot every tenth read: 10 Hz */
#define SYNC_MS 1000u    /* at most this much log lost to a power cut */
#define IMU_MAX_SETS 170u
/* A whole stage: every stage after it then starts on a 4 kB boundary, which
 * a cluster boundary never splits. */
#define HEADER_BYTES STAGE

/* Preallocated per log: 128 MB is about 90 minutes at 1.66 kHz. */
#define EXPAND_BYTES (128u * 1024u * 1024u)
#define EXPAND_MIN (1u * 1024u * 1024u)

#define NEXT_PATH "/logs/next.bin"

static uint8_t ring[RING_SIZE];
static volatile uint32_t r_head; /* the reader */
static volatile uint32_t r_tail; /* the writer */

static hr_stats_t st;
static volatile lsm6ds3_odr_t odr = LSM6DS3_ODR_1660;
static volatile bool odr_changed;
static volatile bool manual; /* the bench asked for a log */
static char manual_reason[16];

static TaskHandle_t h_reader, h_writer;
static StaticTask_t tcb_reader, tcb_writer;
static StackType_t stack_reader[768];
static StackType_t stack_writer[1024];

/* ── The ring ─────────────────────────────────────────────────────── */

static uint32_t ring_used(void) {
    return r_head - r_tail;
}

static void ring_copy_in(uint32_t at, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        ring[(at + i) & RING_MASK] = s[i];
}

static void ring_copy_out(uint32_t at, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t off = at & RING_MASK;
    uint32_t first = n < RING_SIZE - off ? n : RING_SIZE - off;
    memcpy(d, ring + off, first);
    if (first < n)
        memcpy(d + first, ring, n - first);
}

/* The reader's side: a record, whole or not at all. */
static bool put_record(uint8_t type, const void *a, uint32_t na, const void *b, uint32_t nb) {
    uint16_t crc = crc16_update(crc16((const uint8_t *)a, na), (const uint8_t *)b, nb);
    hr_rec_t h = {type, 0, (uint16_t)(na + nb), crc};
    uint32_t n = sizeof(h) + na + nb;
    if (n > RING_SIZE - ring_used()) {
        st.dropped_records++;
        st.dropped_bytes += n;
        return false;
    }
    uint32_t at = r_head;
    ring_copy_in(at, &h, sizeof(h));
    ring_copy_in(at + sizeof(h), a, na);
    if (nb)
        ring_copy_in(at + sizeof(h) + na, b, nb);
    __dmb();
    r_head = at + n;
    uint32_t used = ring_used();
    if (used > st.ring_max)
        st.ring_max = used;
    return true;
}

/* ── The reader ───────────────────────────────────────────────────── */

static lsm6ds3_set_t imu_buf[IMU_MAX_SETS];
static uint8_t ptrace_buf[12 + 16 * sizeof(ptrace_rec_t)];

static uint32_t ptrace_next(void) {
    uint8_t head[12];
    ptrace_read(0xFFFFFFFFu, head, sizeof(head));
    return (uint32_t)head[8] | ((uint32_t)head[9] << 8) | ((uint32_t)head[10] << 16) | ((uint32_t)head[11] << 24);
}

static void read_imu(void) {
    lsm6ds3_read_t r;
    uint32_t t = time_us_32();
    if (!lsm6ds3_read(imu_buf, IMU_MAX_SETS, &r)) {
        st.imu_read_fails++;
        return;
    }
    st.imu_reads++;
    if (r.overrun)
        st.imu_overruns++;
    if (r.backlog > st.imu_backlog_max)
        st.imu_backlog_max = r.backlog;
    if (!r.sets)
        return;
    st.last = imu_buf[r.sets - 1];
    hr_imu_t m = {t, r.backlog, (uint8_t)(r.overrun ? 1 : 0), 0};
    if (put_record(HR_REC_IMU, &m, sizeof(m), imu_buf, r.sets * sizeof(lsm6ds3_set_t)))
        st.imu_sets += r.sets;
}

static uint32_t read_pressure(uint32_t since) {
    int n = ptrace_read(since, ptrace_buf, (int)sizeof(ptrace_buf));
    if (n < 12)
        return since;
    uint32_t next = (uint32_t)ptrace_buf[8] | ((uint32_t)ptrace_buf[9] << 8) | ((uint32_t)ptrace_buf[10] << 16) |
                    ((uint32_t)ptrace_buf[11] << 24);
    for (int o = 12; o + (int)sizeof(ptrace_rec_t) <= n; o += (int)sizeof(ptrace_rec_t)) {
        if (put_record(HR_REC_PRES, ptrace_buf + o, sizeof(ptrace_rec_t), NULL, 0))
            st.pres_records++;
    }
    return next;
}

static void read_flight(void) {
    hr_flight_t f = {
        .t_us = time_us_32(),
        .state = (uint8_t)g_status.state,
        .thrust = (uint8_t)g_status.under_thrust,
        .alt_cm = g_status.altitude_cm,
        .speed_cms = g_status.vertical_speed_cms,
        .pressure_pa = g_status.pressure_pa,
    };
    if (put_record(HR_REC_FLIGHT, &f, sizeof(f), NULL, 0))
        st.flight_records++;
}

static uint32_t pseq, reads;

void hr_reader_begin(void) {
    st.imu_ok = lsm6ds3_start(odr);
    st.odr_hz = lsm6ds3_odr_hz(odr);
    pseq = ptrace_next();
    reads = 0;
}

void hr_reader_step(void) {
    if (odr_changed && !st.logging) {
        odr_changed = false;
        st.imu_ok = lsm6ds3_start(odr);
        st.odr_hz = lsm6ds3_odr_hz(odr);
    }
    if (st.imu_ok)
        read_imu();
    pseq = read_pressure(pseq);
    if (++reads % FLIGHT_EVERY == 0)
        read_flight();
    st.ring_used = ring_used();
}

static void reader_task(void *arg) {
    (void)arg;
    hr_reader_begin();
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(READ_MS));
        hr_reader_step();
        xTaskNotifyGive(h_writer);
    }
}

/* ── The writer ───────────────────────────────────────────────────── */

static FIL fil;
static bool fil_open;
static uint8_t stage[STAGE] __attribute__((aligned(4)));
static uint32_t next_n = 1;
static uint32_t last_sync_ms;

/* Between logs the ring keeps its newest half: whole records are dropped
 * from the tail. */
static void trim(void) {
    while (ring_used() > RING_SIZE / 2) {
        hr_rec_t h;
        ring_copy_out(r_tail, &h, sizeof(h));
        __dmb();
        r_tail += sizeof(h) + h.len;
    }
}

static bool free_name(char *out, size_t cap) {
    FILINFO fi;
    for (; next_n < 10000u; next_n++) {
        snprintf(out, cap, "/logs/hr%04lu.bin", (unsigned long)next_n);
        if (f_stat(out, &fi) != FR_OK)
            return true;
    }
    return false;
}

/* A next.bin that holds a log -- one a power cut left open -- keeps its data
 * under a numbered name. Its tail past the last record is whatever the
 * preallocation held; the decoder stops at the first record that is not one. */
static void recover_next(void) {
    FIL f;
    if (f_open(&f, NEXT_PATH, FA_READ) != FR_OK)
        return;
    uint8_t head[sizeof(hr_rec_t) + 4];
    UINT got = 0;
    bool log = f_read(&f, head, sizeof(head), &got) == FR_OK && got == sizeof(head) && head[0] == HR_REC_HEADER &&
               memcmp(head + sizeof(hr_rec_t), HR_MAGIC, 4) == 0;
    f_close(&f);
    char name[24];
    if (log && free_name(name, sizeof(name)))
        f_rename(NEXT_PATH, name);
}

static void prepare(void) {
    st.preparing = true;
    uint32_t t0 = time_us_32();
    f_mkdir("/logs");
    recover_next();
    uint64_t want = EXPAND_BYTES;
    FATFS *fs;
    DWORD free_cl;
    if (f_getfree("", &free_cl, &fs) == FR_OK) {
        uint64_t free_b = (uint64_t)free_cl * fs->csize * 512u;
        if (free_b / 2u < want)
            want = free_b / 2u;
    }
    if (want < EXPAND_MIN || f_open(&fil, NEXT_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        st.preparing = false;
        return;
    }
    fil_open = true;
    /* Contiguous, so a flight writes no FAT. Without room for that, the file
     * grows as it is written, as any file does. */
    st.expanded_bytes = f_expand(&fil, (FSIZE_t)want, 1) == FR_OK ? (uint32_t)want : 0u;
    f_lseek(&fil, 0);
    st.prepare_us = time_us_32() - t0;
    st.prepared = true;
    st.preparing = false;
}

static bool write_out(const void *buf, uint32_t n) {
    uint32_t t0 = time_us_32();
    UINT put = 0;
    FRESULT r = f_write(&fil, buf, n, &put);
    uint32_t d = time_us_32() - t0;
    st.writes++;
    if (d > st.write_max_us)
        st.write_max_us = d;
    if (r != FR_OK || put != n) {
        st.write_errors++;
        return false;
    }
    st.file_bytes += n;
    st.bytes_total += n;
    return true;
}

_Static_assert(HEADER_BYTES <= STAGE, "the header is built in the stage");

static void begin(const char *reason) {
    hr_header_t h;
    memset(stage, 0, HEADER_BYTES);
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, HR_MAGIC, 4);
    h.version = HR_VERSION;
    h.header_len = sizeof(h);
    h.odr_hz = lsm6ds3_odr_hz(odr);
    h.ug_per_lsb = LSM6DS3_UG_PER_LSB;
    h.mdps_per_lsb = LSM6DS3_MDPS_PER_LSB;
    h.open_us = time_us_32();
    h.open_ms = hal_time_ms();
    strncpy(h.board, PYRO_BOARD_NAME, sizeof(h.board) - 1);
    strncpy(h.reason, reason, sizeof(h.reason) - 1);
    hr_rec_t rh = {HR_REC_HEADER, 0, (uint16_t)(HEADER_BYTES - sizeof(hr_rec_t)), 0};
    memcpy(stage + sizeof(rh), &h, sizeof(h));
    rh.crc = crc16(stage + sizeof(rh), rh.len);
    memcpy(stage, &rh, sizeof(rh));
    st.file_bytes = 0;
    st.logging = true;
    snprintf(st.file, sizeof(st.file), "%s", "logs/next.bin");
    write_out(stage, HEADER_BYTES);
    last_sync_ms = hal_time_ms();
}

static void sync_now(void) {
    uint32_t t0 = time_us_32();
    f_sync(&fil);
    uint32_t d = time_us_32() - t0;
    st.syncs++;
    if (d > st.sync_max_us)
        st.sync_max_us = d;
    last_sync_ms = hal_time_ms();
}

/* The ring's whole content, in stages; the last one short. */
static void drain(bool all) {
    while (ring_used() >= STAGE || (all && ring_used() > 0)) {
        uint32_t n = ring_used() < STAGE ? ring_used() : STAGE;
        ring_copy_out(r_tail, stage, n);
        if (!write_out(stage, n))
            return; /* kept: the next pass retries */
        __dmb();
        r_tail += n;
    }
}

static void finish(void) {
    drain(true);
    f_truncate(&fil);
    f_close(&fil);
    fil_open = false;
    char name[24];
    if (free_name(name, sizeof(name)) && f_rename(NEXT_PATH, name) == FR_OK)
        snprintf(st.file, sizeof(st.file), "%s", name + 1);
    st.logging = false;
    st.prepared = false;
    st.logs++;
}

void hr_writer_step(void) {
    st.card = sd_mounted();
    bool want = manual || hal_log_active();
    if (!st.card) {
        trim();
        return;
    }
    if (!st.logging) {
        if (!fil_open)
            prepare();
        if (want && fil_open)
            begin(manual ? manual_reason : "flight");
        else
            trim();
        return;
    }
    if (!want) {
        finish();
        return;
    }
    drain(false);
    if (hal_time_ms() - last_sync_ms >= SYNC_MS)
        sync_now();
}

static void writer_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        hr_writer_step();
    }
}

#ifdef HR_LOG_TEST
/* A reset mid-log, for the host tests: the file is left as a power cut
 * leaves it -- next.bin, open, its FAT entry as the last sync wrote it -- and
 * the tasks start again. */
void hr_log_test_power_cut(void) {
    fil_open = false;
    st.logging = false;
    st.prepared = false;
    manual = false;
    r_head = r_tail = 0;
}
#endif

/* ── Control ──────────────────────────────────────────────────────── */

void hr_log_create_tasks(void) {
    h_writer = xTaskCreateStaticAffinitySet(writer_task, "hrwrite", sizeof(stack_writer) / sizeof(stack_writer[0]),
                                            NULL, PRIO_P, stack_writer, &tcb_writer, CORE1_ONLY);
    h_reader = xTaskCreateStaticAffinitySet(reader_task, "hrread", sizeof(stack_reader) / sizeof(stack_reader[0]), NULL,
                                            PRIO_P, stack_reader, &tcb_reader, CORE1_ONLY);
}

bool hr_log_start(const char *reason) {
    if (!sd_mounted())
        return false;
    snprintf(manual_reason, sizeof(manual_reason), "%s", reason);
    manual = true;
    return true;
}

void hr_log_stop(void) {
    manual = false;
}

bool hr_log_set_odr(lsm6ds3_odr_t o) {
    if (lsm6ds3_odr_hz(o) == 0 || st.logging)
        return false;
    odr = o;
    odr_changed = true;
    return true;
}

void hr_log_get_stats(hr_stats_t *out) {
    *out = st;
    out->ring_used = ring_used();
}
