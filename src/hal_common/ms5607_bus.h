/*
 * The MS5607 one-shot's bus and clock on the RP2040 [DD-051]: i2c1 and a
 * timer alarm, register by register. test/ms5607_bus.h fakes the same calls.
 *
 * Every call the alarm handler makes is forced inline, so it runs from RAM
 * with the handler: the SDK's i2c and time functions live in flash.
 * support/prove_core0.py fails the build if the handler reaches flash.
 */
#ifndef MS5607_BUS_H
#define MS5607_BUS_H

#include "hardware/irq.h"
#include "hardware/structs/i2c.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#define MS5607_BUS_TIMEOUT_US 2000u /* the longest transfer is about 0.15 ms at 400 kHz */

__force_inline static uint64_t ms5607_bus_now_us(void) {
    uint32_t hi = timer_hw->timerawh;
    for (;;) {
        uint32_t lo = timer_hw->timerawl;
        uint32_t again = timer_hw->timerawh;
        if (again == hi)
            return ((uint64_t)hi << 32) | lo;
        hi = again;
    }
}

/* After a NACK the controller sends STOP itself; either way the transfer is
 * over at STOP_DET. */
__force_inline static bool ms5607_bus_finish(i2c_hw_t *hw, uint32_t t0) {
    while (!(hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
        if (timer_hw->timerawl - t0 > MS5607_BUS_TIMEOUT_US)
            return false;
    }
    bool aborted = hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS;
    (void)hw->clr_intr;
    return !aborted;
}

__force_inline static bool ms5607_bus_command(uint8_t cmd) {
    i2c_hw_t *hw = i2c1_hw;
    uint32_t t0 = timer_hw->timerawl;
    (void)hw->clr_intr;
    hw->data_cmd = I2C_IC_DATA_CMD_STOP_BITS | cmd;
    return ms5607_bus_finish(hw, t0);
}

/* 0x00, then three bytes back: queued at once, the TX FIFO holds sixteen. */
__force_inline static bool ms5607_bus_read_adc(uint32_t *value) {
    i2c_hw_t *hw = i2c1_hw;
    uint32_t t0 = timer_hw->timerawl;
    (void)hw->clr_intr;
    while (hw->rxflr) /* whatever a failed read left behind */
        (void)hw->data_cmd;
    hw->data_cmd = 0x00u;
    hw->data_cmd = I2C_IC_DATA_CMD_RESTART_BITS | I2C_IC_DATA_CMD_CMD_BITS;
    hw->data_cmd = I2C_IC_DATA_CMD_CMD_BITS;
    hw->data_cmd = I2C_IC_DATA_CMD_STOP_BITS | I2C_IC_DATA_CMD_CMD_BITS;
    if (!ms5607_bus_finish(hw, t0) || hw->rxflr < 3u)
        return false;
    uint32_t v = (hw->data_cmd & 0xffu) << 16;
    v |= (hw->data_cmd & 0xffu) << 8;
    v |= hw->data_cmd & 0xffu;
    *value = v;
    return true;
}

/* A target already behind the counter would match only when it wraps, 71
 * minutes on. */
__force_inline static void ms5607_bus_alarm_at(int alarm, uint64_t at_us) {
    uint32_t target = (uint32_t)at_us;
    timer_hw->alarm[alarm] = target;
    if ((int32_t)(target - timer_hw->timerawl) <= 0)
        hw_set_bits(&timer_hw->intf, 1u << (unsigned)alarm);
}

__force_inline static void ms5607_bus_alarm_now(int alarm) {
    hw_set_bits(&timer_hw->intf, 1u << (unsigned)alarm);
}

__force_inline static void ms5607_bus_alarm_ack(int alarm) {
    uint32_t bit = 1u << (unsigned)alarm;
    hw_clear_bits(&timer_hw->intf, bit);
    timer_hw->intr = bit;
}

/* From the loop, once: claims an alarm, addresses the sensor, installs the
 * handler. Nothing else uses i2c1 after this. */
static inline bool ms5607_bus_begin(uint8_t address, irq_handler_t handler, int *alarm) {
    int n = hardware_alarm_claim_unused(false);
    if (n < 0)
        return false;
    i2c1_hw->enable = 0;
    i2c1_hw->tar = address;
    i2c1_hw->enable = 1;
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, (uint)n);
    irq_set_exclusive_handler(irq, handler);
    hw_set_bits(&timer_hw->inte, 1u << (unsigned)n);
    irq_set_enabled(irq, true);
    *alarm = n;
    return true;
}

#endif /* MS5607_BUS_H */
