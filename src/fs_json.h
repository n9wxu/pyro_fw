/*
 * /api/fs: the board's flash store and each of its files [WEB-API-19].
 *
 * Written a file at a time into the connection's work buffer, while the
 * store lists them. A name outside WEB-API-14's characters is shown with
 * each such character a '?': no request can name it by path, and the JSON
 * stays whole and bounded.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FS_JSON_H
#define FS_JSON_H

#include <stdbool.h>
#include <stdint.h>

/* Bytes of file data, free counting pages the cleanup has yet to erase;
 * the sectors the cleanup has erased since the start; the store's sectors,
 * their pages and the file bytes a page holds; cleanup "done", or erasing
 * "fast" (USB-10) or "slow". */
typedef struct {
    uint32_t bytes, in_files, free, cleaned, sectors, sector_pages, page_bytes;
    const char *cleanup;
} fs_space_t;

typedef struct {
    char *buf;
    int cap, len;
    bool first, overflow;
} fs_json_t;

void fs_json_begin(fs_json_t *j, char *buf, int cap, const fs_space_t *s);
/* name bare, reported as its path; size only once closed. */
void fs_json_file(fs_json_t *j, const char *name, uint32_t size, bool closed, bool board);
/* The length, or -1 when the report did not fit. */
int fs_json_end(fs_json_t *j);

/* [WEB-API-20] A chunk of the scan: the length, or -1 when it did not fit. */
int fs_json_scan(char *buf, int cap, uint32_t sector, const uint8_t *erased, int n);

#endif
