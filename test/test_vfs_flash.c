/*
 * The board's own files [DD-100]: src/vfs_flash.c over alfs, on a RAM flash
 * that behaves as NOR does -- a program only clears bits and never crosses a
 * page -- and counts its erases, so a test can hold a write to "erases
 * nothing". A second alfs mount of the same RAM sees what a reset would.
 */
#include "unity.h"
#include "vfs_flash.h"
#include "fs_lock.h"
#include <string.h>

#define SECTOR 4096u
#define PAGE 256u
#define AREA (16u * SECTOR)

static uint8_t chip[AREA];
static unsigned erases, prog_faults;

void fs_lock_init(void) {}
bool fs_lock(void) {
    return true;
}
void fs_unlock(void) {}
bool fs_try_lock(void) {
    return true;
}

static int ram_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    (void)ctx;
    memcpy(buf, chip + addr, len);
    return 0;
}

static int ram_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    (void)ctx;
    if (addr / PAGE != (addr + len - 1u) / PAGE) {
        prog_faults++;
        return -1;
    }
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < len; i++)
        chip[addr + i] &= p[i];
    return 0;
}

static int ram_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    erases++;
    memset(chip + addr, 0xFF, SECTOR);
    return 0;
}

static const alfs_area_t area = {0, AREA, AREA, SECTOR, PAGE};
static const alfs_driver_t drv = {NULL, ram_read, ram_prog, ram_erase};

static uint8_t wbuf[VFS_FLASH_BUF_SIZE], rbuf[VFS_FLASH_BUF_SIZE];

static void fill(uint8_t *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

static int write_whole(const char *name, const uint8_t *d, uint32_t n) {
    vfs_flash_file_t f;
    int rc = vfs_flash_open(&f, name, VFS_FLASH_WRITE, wbuf);
    if (rc != 0)
        return rc;
    if (vfs_flash_write(&f, d, n) != (int)n)
        return -100;
    return vfs_flash_close(&f);
}

static int32_t read_whole(const char *name, uint8_t *out, uint32_t cap) {
    vfs_flash_file_t f;
    int rc = vfs_flash_open(&f, name, VFS_FLASH_READ, rbuf);
    if (rc != 0)
        return rc;
    int32_t total = 0;
    int k;
    while ((k = vfs_flash_read(&f, out + total, cap - (uint32_t)total)) > 0)
        total += k;
    vfs_flash_close(&f);
    return k < 0 ? k : total;
}

/* What a reset would find: a mount of its own over the same chip, which
 * writes nothing. */
static int32_t read_after_reset(const char *name, uint8_t *out, uint32_t cap) {
    static alfs_fs_t fs;
    static alfs_rec_t cache[16];
    static uint8_t scratch[PAGE], blk[PAGE];
    memset(&fs, 0, sizeof(fs));
    if (alfs_fs_mount(&fs, &area, &drv, scratch, cache, 16) != ALFS_OK)
        return -100;
    alfs_reader_t r;
    int rc = alfs_file_open_read(&fs, &r, name, blk, sizeof(blk));
    if (rc != ALFS_OK)
        return rc;
    int32_t total = 0, k;
    while ((k = alfs_file_read(&fs, &r, out + total, cap - (uint32_t)total)) > 0)
        total += k;
    alfs_file_close_read(&fs, &r);
    return k < 0 ? k : total;
}

static void clean_all(void) {
    for (int i = 0; i < 64 && vfs_flash_clean() == 1; i++)
        ;
    TEST_ASSERT_EQUAL(0, vfs_flash_clean());
}

void setUp(void) {}
void tearDown(void) {}

/* Runs first: the module mounts once, as a board does at boot. A mount that
 * cannot write refuses every write; a blank chip mounts with no file. */
static void test_mount_read_only_then_writable(void) {
    memset(chip, 0xFF, sizeof(chip));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_mount(&area, &drv, false));
    TEST_ASSERT_TRUE(vfs_flash_mounted());
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, vfs_flash_open(&f, "config.ini", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(VFS_FLASH_ERR, vfs_flash_open(&f, "config.ini", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(VFS_FLASH_ERR, vfs_flash_remove("config.ini"));
    TEST_ASSERT_EQUAL(0, vfs_flash_clean());
    TEST_ASSERT_EQUAL(0, vfs_flash_mount(&area, &drv, true));
    TEST_ASSERT_EQUAL(0, erases);
}

static void test_write_read_size(void) {
    static uint8_t d[3000], got[4000];
    fill(d, sizeof(d), 1);
    unsigned e0 = erases;
    TEST_ASSERT_EQUAL(0, write_whole("www/app.js", d, sizeof(d)));
    TEST_ASSERT_EQUAL(e0, erases); /* a write erases nothing */
    TEST_ASSERT_EQUAL(sizeof(d), read_whole("www/app.js", got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(d, got, sizeof(d));
    TEST_ASSERT_EQUAL(sizeof(d), vfs_flash_stat_size("www/app.js"));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "www/app.js", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(sizeof(d), vfs_flash_size(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, prog_faults);
}

/* One read fills the buffer across pages, as hal_fs_read_file() reads a
 * configuration file. */
static void test_one_read_takes_the_whole_file(void) {
    static uint8_t d[1000], got[1024];
    fill(d, sizeof(d), 10);
    TEST_ASSERT_EQUAL(0, write_whole("config.ini", d, sizeof(d)));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "config.ini", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(sizeof(d), vfs_flash_read(&f, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(d, got, sizeof(d));
    TEST_ASSERT_EQUAL(0, vfs_flash_read(&f, got, sizeof(got)));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
}

/* The old file reads until the new one closes, and the new one after. */
static void test_replace_whole(void) {
    static uint8_t a[700], b[900], got[1000];
    fill(a, sizeof(a), 2);
    fill(b, sizeof(b), 3);
    TEST_ASSERT_EQUAL(0, write_whole("config.ini", a, sizeof(a)));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "config.ini", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(sizeof(b), vfs_flash_write(&f, b, sizeof(b)));
    TEST_ASSERT_EQUAL(sizeof(a), read_whole("config.ini", got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(a, got, sizeof(a));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(sizeof(b), read_whole("config.ini", got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(b, got, sizeof(b));
    TEST_ASSERT_EQUAL(sizeof(b), read_after_reset("config.ini", got, sizeof(got)));
}

static void test_remove(void) {
    static uint8_t d[100], got[200];
    fill(d, sizeof(d), 4);
    TEST_ASSERT_EQUAL(0, write_whole("pad.mkr", d, sizeof(d)));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("pad.mkr"));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, read_whole("pad.mkr", got, sizeof(got)));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, vfs_flash_remove("pad.mkr"));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, vfs_flash_stat_size("pad.mkr"));
    TEST_ASSERT_EQUAL(ALFS_ENOENT, read_after_reset("pad.mkr", got, sizeof(got)));
}

/* An upload's part file renamed over the file it replaces. */
static void test_rename_over(void) {
    static uint8_t old[500], neu[1200], got[1500];
    fill(old, sizeof(old), 5);
    fill(neu, sizeof(neu), 6);
    TEST_ASSERT_EQUAL(0, write_whole("lua_user.lua", old, sizeof(old)));
    TEST_ASSERT_EQUAL(0, write_whole("lua_user.lua.part", neu, sizeof(neu)));
    TEST_ASSERT_EQUAL(0, vfs_flash_rename("lua_user.lua.part", "lua_user.lua"));
    TEST_ASSERT_EQUAL(sizeof(neu), read_whole("lua_user.lua", got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(neu, got, sizeof(neu));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, read_whole("lua_user.lua.part", got, sizeof(got)));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, vfs_flash_rename("absent", "lua_user.lua"));
}

/* A sync stores every byte written: a reset finds them, though the file
 * never closed and its last page is not full. */
static void test_FLT_LOG_06_a_sync_survives_a_reset(void) {
    static uint8_t d[22 * 60], got[2000];
    fill(d, sizeof(d), 7);
    vfs_flash_file_t f;
    unsigned e0 = erases;
    vfs_flash_remove("flight_log.bin");
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "flight_log.bin", VFS_FLASH_WRITE, wbuf));
    for (uint32_t s = 0; s < 60; s++) {
        TEST_ASSERT_EQUAL(22, vfs_flash_write(&f, d + 22u * s, 22));
        TEST_ASSERT_EQUAL(0, vfs_flash_sync(&f));
        TEST_ASSERT_EQUAL(22u * (s + 1u), read_after_reset("flight_log.bin", got, sizeof(got)));
    }
    TEST_ASSERT_EQUAL(e0, erases); /* the log never erases */
    TEST_ASSERT_EQUAL_MEMORY(d, got, sizeof(d));
    /* a file not closed has no size of its own: it is counted */
    vfs_flash_file_t r;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&r, "flight_log.bin", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(sizeof(d), vfs_flash_size(&r));
    TEST_ASSERT_EQUAL(22, vfs_flash_read(&r, got, 22)); /* the size left the reader at the start */
    TEST_ASSERT_EQUAL_MEMORY(d, got, 22);
    TEST_ASSERT_EQUAL(0, vfs_flash_rewind(&r));
    TEST_ASSERT_EQUAL(22, vfs_flash_read(&r, got, 22));
    TEST_ASSERT_EQUAL_MEMORY(d, got, 22);
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&r));
    TEST_ASSERT_EQUAL(sizeof(d), vfs_flash_stat_size("flight_log.bin"));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(sizeof(d), read_whole("flight_log.bin", got, sizeof(got)));
}

/* Only the cleanup erases: what replaced and removed files left, sector by
 * sector, until no sector holds a dead page; then nothing more. */
static void test_cleanup_frees_what_files_left(void) {
    static uint8_t d[8000], got[9000];
    fill(d, sizeof(d), 8);
    clean_all();
    uint64_t free0, total;
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free0, &total));
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL(0, write_whole("big.bin", d, sizeof(d)));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("big.bin"));
    uint64_t free1;
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free1, &total));
    TEST_ASSERT_EQUAL(free0, free1); /* dead pages count as free: the cleanup frees them */
    unsigned e0 = erases;
    clean_all();
    TEST_ASSERT_TRUE(erases > e0);
    unsigned e1 = erases;
    TEST_ASSERT_EQUAL(0, vfs_flash_clean());
    TEST_ASSERT_EQUAL(e1, erases);
    /* every file still reads */
    TEST_ASSERT_TRUE(read_whole("www/app.js", got, sizeof(got)) > 0);
    TEST_ASSERT_TRUE(read_whole("config.ini", got, sizeof(got)) > 0);
}

/* A reader open holds the cleanup off: it waits, and erases nothing. */
static void test_cleanup_waits_for_readers(void) {
    static uint8_t d[5000];
    fill(d, sizeof(d), 9);
    TEST_ASSERT_EQUAL(0, write_whole("www/index.html", d, sizeof(d)));
    TEST_ASSERT_EQUAL(0, write_whole("www/index.html", d, sizeof(d)));
    vfs_flash_file_t r;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&r, "www/index.html", VFS_FLASH_READ, rbuf));
    unsigned e0 = erases;
    TEST_ASSERT_EQUAL(1, vfs_flash_clean());
    TEST_ASSERT_EQUAL(e0, erases);
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&r));
    clean_all();
}

/* Space is the area's pages less those the files hold: a 3,000-byte file
 * takes a first page of 185 bytes and twelve of 236. */
static void test_space_from_the_files(void) {
    static uint8_t d[3000];
    fill(d, sizeof(d), 11);
    uint64_t free0, free1, total;
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free0, &total));
    TEST_ASSERT_EQUAL((AREA / PAGE - 1u) * 236u, total);
    TEST_ASSERT_EQUAL(0, write_whole("space.bin", d, sizeof(d)));
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free1, &total));
    TEST_ASSERT_EQUAL(free0 - 13u * 236u, free1);
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("space.bin"));
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free1, &total));
    TEST_ASSERT_EQUAL(free0, free1);
}

/* [WEB-API-12] A sync stores its partial page [FLT-LOG-06], so each second
 * of a log takes whole pages, as vfs_flash_synced_cost() says. */
static void test_WEB_API_12_a_synced_second_takes_whole_pages(void) {
    static uint8_t d[1100];
    fill(d, sizeof(d), 12);
    uint64_t free0, free1, total;
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free0, &total));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "log.bin", VFS_FLASH_WRITE, wbuf));
    for (int s = 0; s < 4; s++) {
        TEST_ASSERT_EQUAL((int)sizeof(d), vfs_flash_write(&f, d, sizeof(d)));
        TEST_ASSERT_EQUAL(0, vfs_flash_sync(&f));
    }
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free1, &total));
    TEST_ASSERT_EQUAL(5u * 236u, vfs_flash_synced_cost(sizeof(d)));
    TEST_ASSERT_EQUAL(free0 - 4u * vfs_flash_synced_cost(sizeof(d)), free1);
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("log.bin"));
    clean_all();
}

/* [FLT-LOG-10] A log that runs out of erased pages says so, and erases
 * nothing itself; one cleanup step erases a sector removed files left, and
 * the log goes on into it. */
static void test_FLT_LOG_10_a_log_out_of_erased_pages_says_so_and_a_cleanup_makes_room(void) {
    static uint8_t d[4u * 16u * 236u], chunk[200];
    fill(d, sizeof(d), 13);
    fill(chunk, sizeof(chunk), 14);
    clean_all();
    TEST_ASSERT_EQUAL(0, write_whole("dead.bin", d, sizeof(d)));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("dead.bin"));
    unsigned e0 = erases;
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "flight_log.bin", VFS_FLASH_WRITE, wbuf));
    int rc = 0;
    for (int i = 0; i < 2000 && rc >= 0; i++)
        rc = vfs_flash_write(&f, chunk, sizeof(chunk));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOSPC, rc);
    TEST_ASSERT_EQUAL_MESSAGE(e0, erases, "a write erased");
    vfs_flash_file_t g;
    TEST_ASSERT_EQUAL_MESSAGE(VFS_FLASH_NOSPC, vfs_flash_open(&g, "next_log.bin", VFS_FLASH_WRITE, rbuf),
                              "an open with no erased page says so too");
    TEST_ASSERT_EQUAL(e0, erases);
    TEST_ASSERT_EQUAL(1, vfs_flash_clean());
    TEST_ASSERT_EQUAL(e0 + 1u, erases);
    TEST_ASSERT_EQUAL((int)sizeof(chunk), vfs_flash_write(&f, chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("flight_log.bin"));
    clean_all();
}

/* Writes into a file until the area has no erased page left for it. */
static void fill_to_the_end(vfs_flash_file_t *f, uint32_t seed) {
    static uint8_t chunk[200];
    fill(chunk, sizeof(chunk), seed);
    int rc = 0;
    for (int i = 0; i < 2000 && rc >= 0; i++)
        rc = vfs_flash_write(f, chunk, sizeof(chunk));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOSPC, rc);
}

/* [WEB-API-17] A file open is not deleted; closed, it is, once. */
static void test_WEB_API_17_a_file_open_is_not_deleted(void) {
    static uint8_t d[100];
    fill(d, sizeof(d), 15);
    TEST_ASSERT_EQUAL(0, write_whole("www/del.txt", d, sizeof(d)));
    vfs_flash_file_t r;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&r, "www/del.txt", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(VFS_FLASH_BUSY, vfs_flash_remove("www/del.txt"));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&r));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("www/del.txt"));
    TEST_ASSERT_EQUAL(VFS_FLASH_NOENT, vfs_flash_remove("www/del.txt"));
}

/* [WEB-API-18] An upload's part file that cannot close for want of a page:
 * no restart, and it goes, the file it would have replaced is as it was,
 * its pages are erased, and the space is back. */
static void test_WEB_API_18_a_write_that_cannot_close_is_let_go_without_a_restart(void) {
    static uint8_t old[600], got[1000];
    fill(old, sizeof(old), 16);
    clean_all();
    TEST_ASSERT_EQUAL(0, write_whole("www/page.html", old, sizeof(old)));
    uint64_t free0, free1, total;
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free0, &total));
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "www/page.html.part", VFS_FLASH_WRITE, wbuf));
    fill_to_the_end(&f, 17);
    TEST_ASSERT_EQUAL(VFS_FLASH_NOSPC, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("www/page.html.part"));
    TEST_ASSERT_EQUAL(ALFS_ENOENT, read_after_reset("www/page.html.part", got, sizeof(got)));
    TEST_ASSERT_EQUAL(sizeof(old), read_whole("www/page.html", got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(old, got, sizeof(old));
    unsigned e0 = erases;
    clean_all();
    TEST_ASSERT_TRUE_MESSAGE(erases > e0, "the failed file's pages are erased");
    TEST_ASSERT_EQUAL(0, vfs_flash_space(&free1, &total));
    TEST_ASSERT_EQUAL(free0, free1);
    TEST_ASSERT_EQUAL(0, write_whole("www/page.html.part", old, sizeof(old)));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("www/page.html.part"));
}

/* [WEB-API-18] While another file is open the failed one waits, and a
 * delete asked for meanwhile is done when the last file closes. */
static void test_WEB_API_18_a_failed_write_is_let_go_when_no_other_file_is_open(void) {
    static uint8_t got[1000];
    clean_all();
    int32_t page_size = vfs_flash_stat_size("www/page.html");
    TEST_ASSERT_TRUE(page_size > 0);
    vfs_flash_file_t r, f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&r, "www/page.html", VFS_FLASH_READ, rbuf));
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "www/big.bin.part", VFS_FLASH_WRITE, wbuf));
    fill_to_the_end(&f, 18);
    TEST_ASSERT_EQUAL(VFS_FLASH_NOSPC, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("www/big.bin.part"));
    unsigned e0 = erases;
    TEST_ASSERT_EQUAL(1, vfs_flash_clean());
    TEST_ASSERT_EQUAL(e0, erases);
    TEST_ASSERT_EQUAL(page_size, vfs_flash_read(&r, got, sizeof(got)));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&r));
    TEST_ASSERT_EQUAL(ALFS_ENOENT, read_after_reset("www/big.bin.part", got, sizeof(got)));
    clean_all();
    TEST_ASSERT_TRUE(erases > e0);
    TEST_ASSERT_EQUAL(page_size, read_whole("www/page.html", got, sizeof(got)));
}

/* [WEB-API-18, FLT-LOG-06] A log that cannot close reads as a power cut
 * would have left it, every synced byte kept, and the next log starts. */
static void test_WEB_API_18_a_log_that_cannot_close_reads_as_a_power_cut_left_it(void) {
    static uint8_t rows[22 * 50], got[AREA], again[AREA];
    fill(rows, sizeof(rows), 19);
    vfs_flash_remove("flight_log.bin");
    clean_all();
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "flight_log.bin", VFS_FLASH_WRITE, wbuf));
    for (uint32_t s = 0; s < 50; s++) {
        TEST_ASSERT_EQUAL(22, vfs_flash_write(&f, rows + 22u * s, 22));
        TEST_ASSERT_EQUAL(0, vfs_flash_sync(&f));
    }
    fill_to_the_end(&f, 20);
    TEST_ASSERT_EQUAL(VFS_FLASH_NOSPC, vfs_flash_close(&f));
    int32_t n = read_whole("flight_log.bin", got, sizeof(got));
    TEST_ASSERT_TRUE(n >= (int32_t)sizeof(rows));
    TEST_ASSERT_EQUAL_MEMORY(rows, got, sizeof(rows));
    TEST_ASSERT_EQUAL(n, read_after_reset("flight_log.bin", again, sizeof(again)));
    TEST_ASSERT_EQUAL(n, vfs_flash_stat_size("flight_log.bin"));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("flight_log.bin"));
    clean_all();
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "flight_log.bin", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
    TEST_ASSERT_EQUAL(0, vfs_flash_remove("flight_log.bin"));
}

static void test_names(void) {
    vfs_flash_file_t f;
    TEST_ASSERT_EQUAL(VFS_FLASH_ERR, vfs_flash_open(&f, "", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(VFS_FLASH_ERR, vfs_flash_open(&f, "www/a-name-of-thirty-two-bytes.x", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(0, vfs_flash_open(&f, "www/a-name-of-thirty-one-byte.x", VFS_FLASH_WRITE, wbuf));
    TEST_ASSERT_EQUAL(0, vfs_flash_close(&f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mount_read_only_then_writable);
    RUN_TEST(test_write_read_size);
    RUN_TEST(test_one_read_takes_the_whole_file);
    RUN_TEST(test_replace_whole);
    RUN_TEST(test_remove);
    RUN_TEST(test_rename_over);
    RUN_TEST(test_FLT_LOG_06_a_sync_survives_a_reset);
    RUN_TEST(test_cleanup_frees_what_files_left);
    RUN_TEST(test_cleanup_waits_for_readers);
    RUN_TEST(test_space_from_the_files);
    RUN_TEST(test_WEB_API_12_a_synced_second_takes_whole_pages);
    RUN_TEST(test_FLT_LOG_10_a_log_out_of_erased_pages_says_so_and_a_cleanup_makes_room);
    RUN_TEST(test_WEB_API_17_a_file_open_is_not_deleted);
    RUN_TEST(test_WEB_API_18_a_write_that_cannot_close_is_let_go_without_a_restart);
    RUN_TEST(test_WEB_API_18_a_failed_write_is_let_go_when_no_other_file_is_open);
    RUN_TEST(test_WEB_API_18_a_log_that_cannot_close_reads_as_a_power_cut_left_it);
    RUN_TEST(test_names);
    return UNITY_END();
}
