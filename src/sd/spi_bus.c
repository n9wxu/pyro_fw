/*
 * The SPI bus an SD card and the accelerometer share. See spi_bus.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "spi_bus.h"
#include "board_pins.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

static SemaphoreHandle_t bus_mutex;
static StaticSemaphore_t bus_mutex_buf;

static void cs_output(uint pin) {
    gpio_init(pin);
    gpio_put(pin, 1);
    gpio_set_dir(pin, GPIO_OUT);
}

void spi_bus_init(void) {
    if (bus_mutex)
        return;
    bus_mutex = xSemaphoreCreateMutexStatic(&bus_mutex_buf);
    cs_output(BOARD_PIN_SD_CS);
    cs_output(BOARD_PIN_IMU_CS);
    spi_init(BOARD_SPI_INST, 400000u);
    gpio_set_function(BOARD_PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(BOARD_PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(BOARD_PIN_SPI_MISO, GPIO_FUNC_SPI);
    /* An absent card leaves MISO floating: pulled up it reads 0xFF, no card,
     * where the pad's reset pull-down would read 0x00, a plausible R1. */
    gpio_pull_up(BOARD_PIN_SPI_MISO);

    /* CTRL4_C (13h), I2C_disable, bit 2: written with the LSM6DS3 in SPI
     * mode 3, 1 MHz, before the first SD byte (page 55). */
    spi_bus_setup(1000000u, 1, 1);
    uint8_t w[2] = {0x13, 0x04};
    gpio_put(BOARD_PIN_IMU_CS, 0);
    spi_bus_xfer(w, NULL, 2);
    gpio_put(BOARD_PIN_IMU_CS, 1);
}

bool spi_bus_take(uint32_t timeout_ms) {
    if (!rtos_running())
        return true;
    if (rtos_in_flight_task())
        return false;
    return xSemaphoreTake(bus_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void spi_bus_give(void) {
    if (rtos_running())
        xSemaphoreGive(bus_mutex);
}

uint32_t spi_bus_setup(uint32_t hz, int cpol, int cpha) {
    spi_set_format(BOARD_SPI_INST, 8, cpol ? SPI_CPOL_1 : SPI_CPOL_0, cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
    return spi_set_baudrate(BOARD_SPI_INST, hz);
}

void spi_bus_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n) {
    static const uint8_t ones[64] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    if (tx && rx) {
        spi_write_read_blocking(BOARD_SPI_INST, tx, rx, n);
    } else if (tx) {
        spi_write_blocking(BOARD_SPI_INST, tx, n);
    } else if (rx) {
        /* spi_read_blocking() sends one repeated byte: 0xFF, the idle level
         * a card reads as "no command". */
        spi_read_blocking(BOARD_SPI_INST, 0xFF, rx, n);
    } else {
        while (n > 0) {
            uint32_t k = n < sizeof(ones) ? n : sizeof(ones);
            spi_write_blocking(BOARD_SPI_INST, ones, k);
            n -= k;
        }
    }
}

uint8_t spi_bus_byte(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(BOARD_SPI_INST, &b, &r, 1);
    return r;
}
