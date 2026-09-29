/*
 * The LSM6DS3 accelerometer and gyroscope on the SD card's bus [DD-077].
 *
 * Sources: LSM6DS3_DocID026899_Rev4_2015-04.pdf (the datasheet) and
 * AN4650_LSM6DS3_DocID027415_Rev1.pdf (the application note), both in
 * docs/datasheets/, whose README lists the registers used.
 *
 * Both sensors run at one output rate into the FIFO, which repeats
 * Gx Gy Gz Ax Ay Az (AN4650, 8.5.1), in continuous mode: a full FIFO drops
 * its oldest set and says so (FIFO_OVER_RUN). The FIFO holds 682 sets: 410 ms
 * at 1.66 kHz. No interrupt pin reaches the MCU, so a read takes whole sets
 * only, aligned by FIFO_PATTERN (AN4650, page 88).
 *
 * ±16 g and ±2000 dps: a boost overranges anything less.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LSM6DS3_H
#define LSM6DS3_H

#include <stdbool.h>
#include <stdint.h>

#define LSM6DS3_WHO_AM_I 0x69u
#define LSM6DS3_FIFO_SETS 682u

/* One FIFO set, in the order the FIFO holds it. Little-endian int16, as the
 * FIFO delivers it (FIFO_DATA_OUT_L first). */
typedef struct {
    int16_t g[3];
    int16_t a[3];
} lsm6ds3_set_t;

_Static_assert(sizeof(lsm6ds3_set_t) == 12, "a FIFO set is six 16-bit words");

/* Scale: 16 g full scale is 0.488 mg/LSB, 2000 dps is 70 mdps/LSB
 * (datasheet, page 19, Table 3). */
#define LSM6DS3_UG_PER_LSB 488
#define LSM6DS3_MDPS_PER_LSB 70

/* Output rates both sensors support: 13 * 2^n Hz up to 1.66 kHz (pages 52-53). */
typedef enum {
    LSM6DS3_ODR_104 = 4,
    LSM6DS3_ODR_208 = 5,
    LSM6DS3_ODR_416 = 6,
    LSM6DS3_ODR_833 = 7,
    LSM6DS3_ODR_1660 = 8,
} lsm6ds3_odr_t;

uint32_t lsm6ds3_odr_hz(lsm6ds3_odr_t odr);

/* Reset, check WHO_AM_I, configure, and start the FIFO. False on a part that
 * does not answer 0x69. */
bool lsm6ds3_start(lsm6ds3_odr_t odr);

/* Stops the sensors and the FIFO. */
void lsm6ds3_stop(void);

typedef struct {
    uint32_t sets;    /* sets read */
    bool overrun;     /* the FIFO filled and dropped its oldest set */
    uint16_t backlog; /* words left unread after this read */
} lsm6ds3_read_t;

/* Up to max whole sets, oldest first. Never waits for data: an empty FIFO
 * reads zero sets. Takes the SPI bus; false when it could not. */
bool lsm6ds3_read(lsm6ds3_set_t *out, uint32_t max, lsm6ds3_read_t *r);

#endif
