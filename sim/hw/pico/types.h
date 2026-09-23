/* Host stand-in for pico/types.h. See rp2040_shim.h. */
#ifndef _PICO_TYPES_H
#define _PICO_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned int uint;
typedef struct { uint64_t _private_us_since_boot; } absolute_time_t;
#endif
