/*
 * A MAC drawn from the RNG, and the /serial.txt that keeps it [DD-072].
 *
 * The RP2040 has no hardware RNG. The ring oscillator's random bit is usable
 * while the system runs from the crystal (rp2040-datasheet_2025-02-20.pdf,
 * section 2.17.5, page 223), but no figure is given for its entropy, so many
 * more samples are pooled than the 40 bits a MAC needs, together with the
 * ADC's noisy low bits, and mixed through a 64-bit finaliser.
 *
 * Pure: the samples come from the caller, so the host tests drive it.
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

void mac_pool_init(mac_pool_t *p, uint64_t seed);

/* One sample; any width, any quality. */
void mac_pool_add(mac_pool_t *p, uint32_t sample);

/* 0x02 leads (locally administered, unicast). The last byte is the board's
 * subnet, 192.168.<last>.1: never 0 or 255, which are awkward subnets, nor 1,
 * the commonest home LAN's. */
void mac_pool_draw(mac_pool_t *p, uint8_t out[MAC_BYTES]);

/* /serial.txt: twelve hex digits, then "rng" on a second line when the MAC was
 * drawn rather than assigned. Trailing whitespace is tolerated, since the file
 * may be written by hand. False, and *out untouched, for anything else. */
bool mac_file_parse(const char *text, uint8_t out[MAC_BYTES], bool *drawn);

/* Returns the length written, excluding the terminator. */
int mac_file_format(const uint8_t mac[MAC_BYTES], bool drawn, char *out, int max);

#endif
