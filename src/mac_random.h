/*
 * The board's MAC, from its flash chip's whole unique ID [DD-099], and the
 * /serial.txt that may hold one an operator assigned.
 *
 * The pool is DD-072's, which drew from the ADC and the ring oscillator; it
 * is now fed the ID alone, so one chip gives one MAC at every boot and on a
 * blank flash. The SDK's 8-byte ID is not enough: two XT25F128F chips share
 * those 8 bytes and differ in the last 4 of their 16 (DD-098).
 *
 * Pure: the ID comes from the caller, so the host tests drive it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MAC_RANDOM_H
#define MAC_RANDOM_H

#include <stdbool.h>
#include <stdint.h>

#define MAC_BYTES 6

/* Enough for "XXXXXXXXXXXX\nrng\n" and a terminator. */
#define MAC_FILE_MAX 24

typedef struct {
    uint64_t a, b;
    uint32_t n;
} mac_pool_t;

/* splitmix64's finaliser: every input bit reaches every output bit. */
uint64_t mac_mix64(uint64_t z);

void mac_pool_init(mac_pool_t *p, uint64_t seed);

/* One sample; any width, any quality. */
void mac_pool_add(mac_pool_t *p, uint32_t sample);

/* 0x02 leads (locally administered, unicast). The last byte is the board's
 * subnet, 192.168.<last>.1: never 0 or 255, which are awkward subnets, nor 1,
 * the commonest home LAN's. */
void mac_pool_draw(mac_pool_t *p, uint8_t out[MAC_BYTES]);

/* /serial.txt: twelve hex digits, then "rng" on a second line for a MAC
 * DD-072's firmware drew. Trailing whitespace is tolerated, since the file
 * may be written by hand. False, and *out untouched, for anything else. */
bool mac_file_parse(const char *text, uint8_t out[MAC_BYTES], bool *drawn);

/* The MAC a unique ID of len bytes derives. */
void mac_from_uid(const uint8_t *uid, int len, uint8_t out[MAC_BYTES]);

/* The board's MAC: one an operator assigned in /serial.txt, or else the
 * ID's. text is the file, or NULL for none. True when assigned. */
bool mac_choose(const char *text, const uint8_t *uid, int len, uint8_t out[MAC_BYTES]);

#endif
