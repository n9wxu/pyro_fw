/*
 * The SPI bus an SD card and the accelerometer share [DD-075].
 *
 * One peripheral, two chip selects (board_pins.h). Whoever takes the bus sets
 * its own clock and mode, drives its own chip select, and gives the bus back
 * between transactions -- an SD card holding its busy line for 250 ms must not
 * hold the accelerometer's reads out for as long (sd_card.c).
 *
 * Every transfer here waits on the SPI peripheral itself: bounded by the byte
 * count at the clock rate.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdbool.h>
#include <stdint.h>

/* Pins, the peripheral at 400 kHz, both chip selects high, and the
 * accelerometer's I2C block off -- deselected, the LSM6DS3 listens as an I2C
 * slave on SCK and MOSI (LSM6DS3_DocID026899_Rev4_2015-04.pdf, pages 32, 55).
 * At boot, before anything else uses the bus; a second call does nothing. */
void spi_bus_init(void);

/* False when not had in timeout_ms. Before the scheduler, and on a board
 * with one user, always true. Never from the flight task. */
bool spi_bus_take(uint32_t timeout_ms);
void spi_bus_give(void);

/* cpol/cpha: SPI mode; hz: the clock asked for, which the peripheral rounds
 * down to one it can make. Returns that. */
uint32_t spi_bus_setup(uint32_t hz, int cpol, int cpha);

/* tx NULL sends 0xFF; rx NULL discards. */
void spi_bus_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n);

uint8_t spi_bus_byte(uint8_t b);

#endif
