/* Host stand-in for pico/time.h. See rp2040_shim.h. */
#ifndef _PICO_TIME_H
#define _PICO_TIME_H
#include "pico/types.h"
#include "rp2040_shim.h"

static inline absolute_time_t get_absolute_time(void) {
    absolute_time_t t = {shim_now_us()};
    return t;
}
static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t._private_us_since_boot / 1000u);
}
static inline uint64_t to_us_since_boot(absolute_time_t t) {
    return t._private_us_since_boot;
}
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return (int64_t)to._private_us_since_boot - (int64_t)from._private_us_since_boot;
}
static inline void sleep_us(uint64_t us) { shim_advance_us(us); }
static inline void sleep_ms(uint32_t ms) { shim_advance_us((uint64_t)ms * 1000u); }
static inline void busy_wait_us(uint64_t us) { shim_advance_us(us); }
static inline void busy_wait_us_32(uint32_t us) { shim_advance_us(us); }
#endif
