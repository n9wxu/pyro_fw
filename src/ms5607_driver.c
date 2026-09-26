/*
 * MS5607-02BA03 pressure sensor driver
 * I2C address: 0x76 or 0x77
 *
 * SPDX-License-Identifier: MIT
 */
#include "ms5607_driver.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/structs/i2c.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include <stdio.h>

#define I2C_PORT i2c1

#define MS5607_ADDR_CSB_LOW 0x76
#define MS5607_ADDR_CSB_HIGH 0x77

#define MS5607_CMD_RESET 0x1E
#define MS5607_CMD_CONV_D1 0x48 // Pressure conversion (OSR=4096)
#define MS5607_CMD_CONV_D2 0x58 // Temperature conversion (OSR=4096)
#define MS5607_CMD_ADC_READ 0x00
#define MS5607_CMD_PROM_READ 0xA0 // Base address for PROM

static uint8_t ms5607_addr = 0;
static uint16_t prom[8];

static bool ms5607_write_cmd(uint8_t cmd) {
    return i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, false) == 1;
}

static bool ms5607_read_adc_internal(uint32_t *value) {
    uint8_t cmd = MS5607_CMD_ADC_READ;
    uint8_t data[3];
    if (i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, true) != 1)
        return false;
    if (i2c_read_blocking(I2C_PORT, ms5607_addr, data, 3, false) != 3)
        return false;
    *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    return true;
}

static bool ms5607_read_raw(uint32_t *value) {
    return ms5607_read_adc_internal(value);
}

bool ms5607_compensate(uint32_t d1, uint32_t d2, pressure_reading_t *out) {
    ms5607_compensate_prom(prom, d1, d2, out);
    return true;
}

static bool ms5607_read_prom(void) {
    for (int i = 0; i < 8; i++) {
        uint8_t cmd = MS5607_CMD_PROM_READ + (i * 2);
        uint8_t data[2];

        if (i2c_write_blocking(I2C_PORT, ms5607_addr, &cmd, 1, true) != 1)
            return false;

        if (i2c_read_blocking(I2C_PORT, ms5607_addr, data, 2, false) != 2)
            return false;

        prom[i] = ((uint16_t)data[0] << 8) | data[1];
    }
    return true;
}

bool ms5607_detect(void) {
    // Try both possible addresses
    for (uint8_t addr = MS5607_ADDR_CSB_LOW; addr <= MS5607_ADDR_CSB_HIGH; addr++) {
        ms5607_addr = addr;

        // Reset
        if (!ms5607_write_cmd(MS5607_CMD_RESET))
            continue;

        sleep_ms(10);

        // Read PROM
        if (ms5607_read_prom()) {
            // Verify PROM CRC (simplified check - just verify non-zero)
            if (prom[0] != 0 && prom[0] != 0xFFFF)
                return true;
        }
    }

    return false;
}

/* Synchronous (blocking) read — used at init/detect time only. */
bool ms5607_read(pressure_reading_t *reading) {
    uint32_t d1, d2;
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D1))
        return false;
    sleep_ms(MS5607_CONV_MS);
    if (!ms5607_read_raw(&d1))
        return false;
    if (!ms5607_write_cmd(MS5607_CMD_CONV_D2))
        return false;
    sleep_ms(MS5607_CONV_MS);
    if (!ms5607_read_raw(&d2))
        return false;
    return ms5607_compensate(d1, d2, reading);
}

/* ── One-shot conversion [DD-051] ─────────────────────────────────────
 *
 * The loop commands a conversion and arms a hardware alarm for its end; the
 * alarm's handler reads the ADC and leaves the result for the next loop.
 *
 * The handler, and everything it touches, lives in RAM. An alarm can come due
 * while core0 is inside a flash erase: littlefs_driver.c holds interrupts off
 * across every erase and program, so it runs once the erase is over, but from
 * RAM it could not fault even were that ever not so. The SDK's i2c functions
 * live in flash, so the handler drives i2c1's registers itself. The TAR is
 * already this sensor's: the loop's command set it, and nothing else uses the
 * bus meanwhile. support/prove_core0.py fails the build if the handler, or
 * anything it calls or loads, is in flash. */
#define MS5607_BUS_TIMEOUT_US 2000u /* a read takes about 0.6 ms at 100 kHz */

static struct {
    volatile bool busy;  /* commanded; the one-shot has not read it yet */
    volatile bool ready; /* read; the loop has not taken it yet */
    bool temperature;
    bool ok;
    uint32_t raw;
    uint64_t command_us;
    int alarm; /* -1: not begun */
} oneshot = {.alarm = -1};

/* 0x00 then three bytes back, queued at once: the TX FIFO holds sixteen. */
static bool __noinline __not_in_flash_func(ms5607_adc_read_ram)(uint32_t *value) {
    i2c_hw_t *hw = i2c1_hw;
    uint32_t t0 = timer_hw->timerawl;
    hw->data_cmd = MS5607_CMD_ADC_READ;
    hw->data_cmd = I2C_IC_DATA_CMD_RESTART_BITS | I2C_IC_DATA_CMD_CMD_BITS;
    hw->data_cmd = I2C_IC_DATA_CMD_CMD_BITS;
    hw->data_cmd = I2C_IC_DATA_CMD_STOP_BITS | I2C_IC_DATA_CMD_CMD_BITS;
    while (hw->rxflr < 3u) {
        if ((hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) || timer_hw->timerawl - t0 > MS5607_BUS_TIMEOUT_US) {
            (void)hw->clr_intr; /* the abort, and the STOP the hardware sent after it */
            return false;
        }
    }
    uint32_t v = (hw->data_cmd & 0xffu) << 16;
    v |= (hw->data_cmd & 0xffu) << 8;
    v |= hw->data_cmd & 0xffu;
    while (!(hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
        if (timer_hw->timerawl - t0 > MS5607_BUS_TIMEOUT_US)
            return false;
    }
    (void)hw->clr_stop_det;
    *value = v;
    return true;
}

static void __noinline __not_in_flash_func(ms5607_alarm_isr)(void) {
    uint32_t bit = 1u << (unsigned)oneshot.alarm;
    hw_clear_bits(&timer_hw->intf, bit);
    timer_hw->intr = bit;
    uint32_t raw = 0;
    oneshot.ok = ms5607_adc_read_ram(&raw);
    oneshot.raw = raw;
    __dmb();
    oneshot.ready = true;
    oneshot.busy = false;
}

bool ms5607_async_begin(void) {
    if (oneshot.alarm >= 0)
        return true;
    int n = hardware_alarm_claim_unused(false);
    if (n < 0)
        return false;
    oneshot.alarm = n;
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, (uint)n);
    irq_set_exclusive_handler(irq, ms5607_alarm_isr);
    hw_set_bits(&timer_hw->inte, 1u << (unsigned)n);
    irq_set_enabled(irq, true);
    return true;
}

ms5607_start_t ms5607_async_start(bool temperature) {
    if (oneshot.alarm < 0)
        return MS5607_BUS_ERROR;
    if (oneshot.busy)
        return MS5607_BUSY;
    if (!ms5607_write_cmd(temperature ? MS5607_CMD_CONV_D2 : MS5607_CMD_CONV_D1))
        return MS5607_BUS_ERROR;
    uint64_t now = time_us_64();
    oneshot.command_us = now;
    oneshot.temperature = temperature;
    oneshot.busy = true;
    __dmb();
    uint32_t bit = 1u << (unsigned)oneshot.alarm;
    uint32_t target = (uint32_t)now + MS5607_CONV_DONE_US;
    timer_hw->alarm[oneshot.alarm] = target;
    /* An alarm behind the counter fires only when it wraps, 71 minutes on. */
    if ((int32_t)(target - timer_hw->timerawl) <= 0)
        hw_set_bits(&timer_hw->intf, bit);
    return MS5607_STARTED;
}

bool ms5607_async_take(ms5607_conversion_t *out) {
    if (!oneshot.ready)
        return false;
    __dmb();
    out->raw = oneshot.raw;
    out->ok = oneshot.ok;
    out->temperature = oneshot.temperature;
    out->command_us = oneshot.command_us;
    oneshot.ready = false;
    return true;
}
