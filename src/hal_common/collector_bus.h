/*
 * The pressure collector's bus and clock on the RP2040 [DD-093]: an I2C
 * controller, a timer alarm and the two pads, register by register.
 * test/collector_bus.h fakes the same calls.
 *
 * No call waits. A transfer is queued whole, which the sixteen-entry TX FIFO
 * allows, and its outcome is read at a later alarm. Every call the alarm's
 * handler makes is forced inline, so it runs from RAM with the handler: the
 * SDK's i2c, gpio and time functions live in flash. support/prove_core0.py
 * fails the build if the handler reaches flash.
 *
 * Figures are from docs/datasheets/rp2040-datasheet_2025-02-20.pdf, section
 * 4.3.
 */
#ifndef COLLECTOR_BUS_H
#define COLLECTOR_BUS_H

#include "flash_op.h"
#include "pressure_collector.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/resets.h"
#include "hardware/structs/i2c.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#define COLLECTOR_BUS_HANDLER __attribute__((optimize("no-jump-tables", "no-tree-loop-distribute-patterns")))

/* What i2c_init() and i2c_set_baudrate() wrote, to write again after the
 * controller's reset. */
typedef struct {
    uint32_t con, tar, tx_tl, rx_tl, dma_cr;
    uint32_t ss_scl_hcnt, ss_scl_lcnt, fs_scl_hcnt, fs_scl_lcnt, fs_spklen, sda_hold;
} collector_bus_setup_t;

static struct {
    i2c_hw_t *hw;
    uint32_t reset_bit;
    uint32_t scl_mask, sda_mask;
    uint8_t scl_pin, sda_pin;
    uint32_t alarm_bit;
    uint8_t alarm;
    collector_bus_setup_t setup;
} collector_bus;

__force_inline static uint64_t collector_bus_now_us(void) {
    uint32_t hi = timer_hw->timerawh;
    for (;;) {
        uint32_t lo = timer_hw->timerawl;
        uint32_t again = timer_hw->timerawh;
        if (again == hi)
            return ((uint64_t)hi << 32) | lo;
        hi = again;
    }
}

/* [DD-068] Compared across a conversion: a change is a flash operation that
 * ran beside it. */
__force_inline static uint32_t collector_bus_flash_ops(void) {
    return flash_op_seq;
}

/* ── The alarm ────────────────────────────────────────────────────── */

__force_inline static void collector_bus_alarm_now(void) {
    hw_set_bits(&timer_hw->intf, collector_bus.alarm_bit);
}

/* A target already behind the counter would match only when it wraps, 71
 * minutes on. */
__force_inline static void collector_bus_alarm_at(uint64_t at_us) {
    uint32_t target = (uint32_t)at_us;
    timer_hw->alarm[collector_bus.alarm] = target;
    if ((int32_t)(target - timer_hw->timerawl) <= 0)
        collector_bus_alarm_now();
}

__force_inline static void collector_bus_alarm_ack(void) {
    hw_clear_bits(&timer_hw->intf, collector_bus.alarm_bit);
    timer_hw->intr = collector_bus.alarm_bit;
}

/* The task's side of the queue: the handler does not run between these. */
__force_inline static void collector_bus_hold_off(void) {
    hw_clear_bits(&timer_hw->inte, collector_bus.alarm_bit);
    __isb();
}

__force_inline static void collector_bus_let_in(void) {
    hw_set_bits(&timer_hw->inte, collector_bus.alarm_bit);
}

/* ── Transfers ────────────────────────────────────────────────────── */

/* Reading clr_intr also ends the TX FIFO's flush after an abort (4.3.10). */
__force_inline static void collector_bus_fresh(i2c_hw_t *hw) {
    (void)hw->clr_intr;
    while (hw->rxflr)
        (void)hw->data_cmd;
}

__force_inline static void collector_bus_write(const uint8_t *bytes, uint8_t len) {
    i2c_hw_t *hw = collector_bus.hw;
    collector_bus_fresh(hw);
    for (uint8_t i = 0; i < len; i++)
        hw->data_cmd = bytes[i] | (i + 1u == len ? I2C_IC_DATA_CMD_STOP_BITS : 0u);
}

__force_inline static void collector_bus_read(uint8_t from_register, uint8_t len) {
    i2c_hw_t *hw = collector_bus.hw;
    collector_bus_fresh(hw);
    hw->data_cmd = from_register;
    for (uint8_t i = 0; i < len; i++)
        hw->data_cmd = I2C_IC_DATA_CMD_CMD_BITS | (i == 0u ? I2C_IC_DATA_CMD_RESTART_BITS : 0u) |
                       (i + 1u == len ? I2C_IC_DATA_CMD_STOP_BITS : 0u);
}

/* How the queued transfer ended, and its bytes. One that has not ended is
 * given up: the controller is told to let the bus go (4.3.10). */
__force_inline static collector_cause_t collector_bus_result(uint8_t *data, uint8_t len) {
    i2c_hw_t *hw = collector_bus.hw;
    uint32_t raised = hw->raw_intr_stat;
    if (raised & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
        uint32_t why = hw->tx_abrt_source;
        (void)hw->clr_tx_abrt;
        if (why & I2C_IC_TX_ABRT_SOURCE_ABRT_7B_ADDR_NOACK_BITS)
            return COLLECTOR_ADDRESS_NACK;
        if (why & I2C_IC_TX_ABRT_SOURCE_ABRT_TXDATA_NOACK_BITS)
            return COLLECTOR_DATA_NACK;
        return COLLECTOR_LINE_HELD;
    }
    if (!(raised & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS) || hw->rxflr < len) {
        hw_set_bits(&hw->enable, I2C_IC_ENABLE_ABORT_BITS);
        return COLLECTOR_TIMEOUT;
    }
    for (uint8_t i = 0; i < len; i++)
        data[i] = (uint8_t)hw->data_cmd;
    return COLLECTOR_OK;
}

/* ── The lines, for a bus clear ───────────────────────────────────── */

/* Both lines are driven low or let go, never driven high. */
__force_inline static void collector_bus_line(uint32_t mask, bool high) {
    if (high)
        sio_hw->gpio_oe_clr = mask;
    else
        sio_hw->gpio_oe_set = mask;
}

__force_inline static void collector_bus_scl(bool high) {
    collector_bus_line(collector_bus.scl_mask, high);
}

__force_inline static void collector_bus_sda(bool high) {
    collector_bus_line(collector_bus.sda_mask, high);
}

/* The pads leave the controller, both let go, and the controller is reset:
 * one left holding SCL has no other way out (4.3.13.2). */
__force_inline static void collector_bus_lines_take(void) {
    uint32_t both = collector_bus.scl_mask | collector_bus.sda_mask;
    sio_hw->gpio_oe_clr = both;
    sio_hw->gpio_clr = both;
    io_bank0_hw->io[collector_bus.scl_pin].ctrl = GPIO_FUNC_SIO;
    io_bank0_hw->io[collector_bus.sda_pin].ctrl = GPIO_FUNC_SIO;
    hw_set_bits(&resets_hw->reset, collector_bus.reset_bit);
    hw_clear_bits(&resets_hw->reset, collector_bus.reset_bit);
}

/* The controller as it was set up, and the pads back to it. The reset has
 * had the whole bus clear to finish in. */
__force_inline static void collector_bus_lines_give(void) {
    i2c_hw_t *hw = collector_bus.hw;
    const collector_bus_setup_t *s = &collector_bus.setup;
    hw->enable = 0;
    hw->con = s->con;
    hw->tar = s->tar;
    hw->tx_tl = s->tx_tl;
    hw->rx_tl = s->rx_tl;
    hw->dma_cr = s->dma_cr;
    hw->ss_scl_hcnt = s->ss_scl_hcnt;
    hw->ss_scl_lcnt = s->ss_scl_lcnt;
    hw->fs_scl_hcnt = s->fs_scl_hcnt;
    hw->fs_scl_lcnt = s->fs_scl_lcnt;
    hw->fs_spklen = s->fs_spklen;
    hw->sda_hold = s->sda_hold;
    hw->enable = 1;
    io_bank0_hw->io[collector_bus.sda_pin].ctrl = GPIO_FUNC_I2C;
    io_bank0_hw->io[collector_bus.scl_pin].ctrl = GPIO_FUNC_I2C;
}

/* From a task, once: claims an alarm, addresses the part, notes the
 * controller's setup and installs the handler. Nothing else uses this
 * controller afterwards. */
static inline bool collector_bus_begin(const collector_wiring_t *w, irq_handler_t handler) {
    int n = hardware_alarm_claim_unused(false);
    if (n < 0)
        return false;
    i2c_hw_t *hw = w->i2c == 0 ? i2c0_hw : i2c1_hw;
    hw->enable = 0;
    hw->tar = w->address;
    hw->enable = 1;
    collector_bus.hw = hw;
    collector_bus.reset_bit = w->i2c == 0 ? RESETS_RESET_I2C0_BITS : RESETS_RESET_I2C1_BITS;
    collector_bus.scl_pin = w->scl_pin;
    collector_bus.sda_pin = w->sda_pin;
    collector_bus.scl_mask = 1u << w->scl_pin;
    collector_bus.sda_mask = 1u << w->sda_pin;
    collector_bus.alarm = (uint8_t)n;
    collector_bus.alarm_bit = 1u << (unsigned)n;
    collector_bus.setup = (collector_bus_setup_t){hw->con,         hw->tar,         hw->tx_tl,       hw->rx_tl,
                                                  hw->dma_cr,      hw->ss_scl_hcnt, hw->ss_scl_lcnt, hw->fs_scl_hcnt,
                                                  hw->fs_scl_lcnt, hw->fs_spklen,   hw->sda_hold};
    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, (uint)n);
    irq_set_exclusive_handler(irq, handler);
    hw_set_bits(&timer_hw->inte, collector_bus.alarm_bit);
    irq_set_enabled(irq, true);
    return true;
}

#endif /* COLLECTOR_BUS_H */
