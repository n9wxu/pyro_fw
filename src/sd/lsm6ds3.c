/*
 * The LSM6DS3 on the SD card's bus. See lsm6ds3.h; register pages are the
 * datasheet's.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lsm6ds3.h"
#include "spi_bus.h"
#include "board_pins.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#define REG_FIFO_CTRL3 0x08u
#define REG_FIFO_CTRL5 0x0Au
#define REG_WHO_AM_I 0x0Fu
#define REG_CTRL1_XL 0x10u
#define REG_CTRL2_G 0x11u
#define REG_CTRL3_C 0x12u
#define REG_CTRL4_C 0x13u
#define REG_FIFO_STATUS1 0x3Au
#define REG_FIFO_DATA_OUT_L 0x3Eu

#define CTRL3_IF_INC 0x04u /* page 54 */
#define CTRL3_SW_RESET 0x01u
#define CTRL4_I2C_DISABLE 0x04u     /* page 55 */
#define FS_XL_16G (0x1u << 2)       /* page 52 */
#define FS_G_2000 (0x3u << 2)       /* page 53 */
#define FIFO_BOTH_UNDECIMATED 0x09u /* DEC_FIFO_GYRO 001, DEC_FIFO_XL 001, page 46 */
#define FIFO_CONTINUOUS 0x6u        /* page 49 */
#define ST2_OVER_RUN 0x40u          /* page 69 */
#define ST2_EMPTY 0x10u

#define WORDS_PER_SET 6u

static void bus_begin(void) {
    spi_bus_setup(BOARD_IMU_SPI_HZ, 1, 1); /* mode 3, page 34 */
    gpio_put(BOARD_PIN_IMU_CS, 0);
}

static void bus_end(void) {
    gpio_put(BOARD_PIN_IMU_CS, 1);
}

static bool write_reg(uint8_t reg, uint8_t val) {
    if (!spi_bus_take(200))
        return false;
    bus_begin();
    uint8_t w[2] = {reg, val};
    spi_bus_xfer(w, NULL, 2);
    bus_end();
    spi_bus_give();
    return true;
}

static bool read_regs(uint8_t reg, uint8_t *out, uint32_t n) {
    if (!spi_bus_take(200))
        return false;
    bus_begin();
    uint8_t a = (uint8_t)(0x80u | reg);
    spi_bus_xfer(&a, NULL, 1);
    spi_bus_xfer(NULL, out, n);
    bus_end();
    spi_bus_give();
    return true;
}

uint32_t lsm6ds3_odr_hz(lsm6ds3_odr_t odr) {
    /* 13 Hz at code 1, doubling: 104, 208, 416, 833, 1660 (pages 52-53). */
    static const uint16_t hz[] = {0, 13, 26, 52, 104, 208, 416, 833, 1660};
    return (unsigned)odr < sizeof(hz) / sizeof(hz[0]) ? hz[odr] : 0u;
}

bool lsm6ds3_start(lsm6ds3_odr_t odr) {
    uint8_t who = 0;
    if (!read_regs(REG_WHO_AM_I, &who, 1) || who != LSM6DS3_WHO_AM_I)
        return false;
    write_reg(REG_CTRL3_C, CTRL3_IF_INC | CTRL3_SW_RESET);
    /* SW_RESET clears itself once the reset is done (page 54). Bounded:
     * a part still resetting after 10 ms is not coming back. */
    uint32_t t0 = time_us_32();
    uint8_t c3 = CTRL3_SW_RESET;
    while ((c3 & CTRL3_SW_RESET) && time_us_32() - t0 < 10000u)
        read_regs(REG_CTRL3_C, &c3, 1);
    /* The reset re-enables I2C: off again before any SD traffic. BDU off, as
     * the FIFO status registers ask (page 69); IF_INC on for bursts. */
    write_reg(REG_CTRL4_C, CTRL4_I2C_DISABLE);
    write_reg(REG_CTRL3_C, CTRL3_IF_INC);
    write_reg(REG_CTRL1_XL, (uint8_t)(((unsigned)odr << 4) | FS_XL_16G));
    write_reg(REG_CTRL2_G, (uint8_t)(((unsigned)odr << 4) | FS_G_2000));
    write_reg(REG_FIFO_CTRL3, FIFO_BOTH_UNDECIMATED);
    /* Bypass first, which empties the FIFO, then continuous at the ODR. */
    write_reg(REG_FIFO_CTRL5, 0);
    return write_reg(REG_FIFO_CTRL5, (uint8_t)(((unsigned)odr << 3) | FIFO_CONTINUOUS));
}

void lsm6ds3_stop(void) {
    write_reg(REG_FIFO_CTRL5, 0);
    write_reg(REG_CTRL1_XL, 0);
    write_reg(REG_CTRL2_G, 0);
}

bool lsm6ds3_read(lsm6ds3_set_t *out, uint32_t max, lsm6ds3_read_t *r) {
    r->sets = 0;
    r->overrun = false;
    r->backlog = 0;
    if (!spi_bus_take(200))
        return false;
    bus_begin();
    uint8_t a = (uint8_t)(0x80u | REG_FIFO_STATUS1);
    uint8_t st[4];
    spi_bus_xfer(&a, NULL, 1);
    spi_bus_xfer(NULL, st, 4);
    bus_end();

    uint32_t words = ((uint32_t)(st[1] & 0x0Fu) << 8) | st[0];
    uint32_t pattern = ((uint32_t)(st[3] & 0x03u) << 8) | st[2];
    r->overrun = (st[1] & ST2_OVER_RUN) != 0;
    if ((st[1] & ST2_EMPTY) || words == 0) {
        spi_bus_give();
        return true;
    }

    /* Whole sets only: words to the next Gx are read and dropped. Their
     * register addresses roll over within the FIFO output (AN4650, page 89). */
    uint32_t skip = (WORDS_PER_SET - pattern % WORDS_PER_SET) % WORDS_PER_SET;
    if (skip > words)
        skip = words;
    uint32_t sets = (words - skip) / WORDS_PER_SET;
    if (sets > max)
        sets = max;

    a = (uint8_t)(0x80u | REG_FIFO_DATA_OUT_L);
    bus_begin();
    spi_bus_xfer(&a, NULL, 1);
    if (skip) {
        uint8_t junk[2 * WORDS_PER_SET];
        spi_bus_xfer(NULL, junk, 2u * skip);
    }
    if (sets)
        spi_bus_xfer(NULL, (uint8_t *)out, sets * sizeof(lsm6ds3_set_t));
    bus_end();
    spi_bus_give();

    r->sets = sets;
    r->backlog = (uint16_t)(words - skip - sets * WORDS_PER_SET);
    return true;
}
