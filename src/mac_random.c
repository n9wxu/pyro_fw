/*
 * The board's MAC. See mac_random.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mac_random.h"
#include <string.h>

uint64_t mac_mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void mac_pool_init(mac_pool_t *p, uint64_t seed) {
    p->a = mac_mix64(seed ^ 0x9E3779B97F4A7C15ull);
    p->b = mac_mix64(p->a ^ 0x632BE59BD9B4E019ull);
    p->n = 0;
}

void mac_pool_add(mac_pool_t *p, uint32_t sample) {
    p->n++;
    p->a = mac_mix64(p->a ^ ((uint64_t)sample << 32 | p->n));
    p->b = mac_mix64(p->b + p->a);
}

static bool bad_subnet(uint8_t o) {
    return o == 0u || o == 1u || o == 255u;
}

void mac_pool_draw(mac_pool_t *p, uint8_t out[MAC_BYTES]) {
    uint64_t r = mac_mix64(p->a ^ mac_mix64(p->b));
    out[0] = 0x02u;
    for (int i = 1; i < MAC_BYTES; i++) {
        out[i] = (uint8_t)(r >> (8 * i));
    }
    while (bad_subnet(out[MAC_BYTES - 1])) {
        r = mac_mix64(r);
        out[MAC_BYTES - 1] = (uint8_t)r;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool mac_file_parse(const char *text, uint8_t out[MAC_BYTES], bool *drawn) {
    uint8_t m[MAC_BYTES];
    for (int i = 0; i < MAC_BYTES; i++) {
        int hi = hexval(text[i * 2]);
        int lo = hi < 0 ? -1 : hexval(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        m[i] = (uint8_t)((hi << 4) | lo);
    }
    /* A multicast source address is not something to debug later. */
    if (m[0] & 0x01u)
        return false;
    const char *s = text + MAC_BYTES * 2;
    bool rng = false;
    while (is_space(*s))
        s++;
    if (strncmp(s, "rng", 3) == 0) {
        rng = true;
        s += 3;
        while (is_space(*s))
            s++;
    }
    if (*s != '\0')
        return false;
    memcpy(out, m, MAC_BYTES);
    if (drawn)
        *drawn = rng;
    return true;
}

/* Big-endian words, the last zero-padded; the length seeds the pool, so an
 * ID is not its own first half. */
void mac_from_uid(const uint8_t *uid, int len, uint8_t out[MAC_BYTES]) {
    mac_pool_t p;
    mac_pool_init(&p, (uint64_t)len);
    for (int i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (int j = 0; j < 4; j++)
            w = (w << 8) | (i + j < len ? uid[i + j] : 0u);
        mac_pool_add(&p, w);
    }
    mac_pool_draw(&p, out);
}

bool mac_choose(const char *text, const uint8_t *uid, int len, uint8_t out[MAC_BYTES]) {
    bool drawn = true;
    if (text && mac_file_parse(text, out, &drawn) && !drawn)
        return true;
    mac_from_uid(uid, len, out);
    return false;
}
