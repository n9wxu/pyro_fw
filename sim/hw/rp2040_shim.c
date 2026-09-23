/*
 * Host stand-in for the Pico SDK surface the pyro board files use.
 * See rp2040_shim.h for the design and for why the clock matters.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rp2040_shim.h"
#include "plant.h"

#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include <string.h>
#include <stdint.h>

/* ── Clock ───────────────────────────────────────────────────────── */

static uint64_t now_us;

uint64_t shim_now_us(void) { return now_us; }

/* ── ADC ─────────────────────────────────────────────────────────── */

/* One conversion is 96 cycles of the 48 MHz ADC clock. */
#define ADC_CONV_US 2

static uint adc_input;
static bool adc_fifo_en;
static bool adc_running;
static double adc_sample_interval_us = ADC_CONV_US;
static double adc_accum_us; /* time owed to the free-running converter */

static adc_hw_t adc_hw_storage;
adc_hw_t *const adc_hw = &adc_hw_storage;

void adc_init(void) {
    adc_input = 0;
    adc_fifo_en = false;
    adc_running = false;
    adc_sample_interval_us = ADC_CONV_US;
    adc_accum_us = 0.0;
}
void adc_gpio_init(uint gpio) { (void)gpio; }
void adc_select_input(uint input) { adc_input = input; }
uint adc_get_selected_input(void) { return adc_input; }
void adc_set_temp_sensor_enabled(bool enable) { (void)enable; }

uint16_t adc_read(void) {
    /* A one-shot conversion. Costing it virtual time is what lets a
     * polling loop measure a real interval. */
    shim_advance_us(ADC_CONV_US);
    return plant_adc_counts((int)adc_input);
}

void adc_fifo_setup(bool en, bool dreq_en, uint16_t dreq_thresh, bool err_in_fifo, bool byte_shift) {
    (void)dreq_en; (void)dreq_thresh; (void)err_in_fifo; (void)byte_shift;
    adc_fifo_en = en;
}
void adc_fifo_drain(void) { adc_accum_us = 0.0; }
bool adc_fifo_is_empty(void) { return true; }
uint16_t adc_fifo_get(void) { return plant_adc_counts((int)adc_input); }

void adc_set_clkdiv(float clkdiv) {
    /* The SDK's divider counts in ADC clocks: a sample every (clkdiv + 1)
     * ticks of 48 MHz. 0 means back-to-back conversions. */
    double ticks = (double)clkdiv + 1.0;
    double us = ticks / 48.0;
    adc_sample_interval_us = (us < ADC_CONV_US) ? ADC_CONV_US : us;
}

void adc_run(bool run) {
    adc_running = run;
    adc_accum_us = 0.0;
}

/* ── DMA ─────────────────────────────────────────────────────────── */

#define DMA_N_CHANNELS 12

static struct {
    bool      claimed;
    bool      busy;
    bool      from_adc;
    uint16_t *dst;
    uint      count;
    uint      done;
} dma_ch[DMA_N_CHANNELS];

int dma_claim_unused_channel(bool required) {
    for (int i = 0; i < DMA_N_CHANNELS; i++)
        if (!dma_ch[i].claimed) { dma_ch[i].claimed = true; return i; }
    return required ? 0 : -1;
}
void dma_channel_unclaim(uint channel) {
    if (channel < DMA_N_CHANNELS) memset(&dma_ch[channel], 0, sizeof(dma_ch[0]));
}
dma_channel_config dma_channel_get_default_config(uint channel) {
    (void)channel;
    dma_channel_config c = {true, true, 0, DMA_SIZE_32};
    return c;
}
void channel_config_set_transfer_data_size(dma_channel_config *c, enum_dma_channel_transfer_size s) { c->size = s; }
void channel_config_set_read_increment(dma_channel_config *c, bool i) { c->read_incr = i; }
void channel_config_set_write_increment(dma_channel_config *c, bool i) { c->write_incr = i; }
void channel_config_set_dreq(dma_channel_config *c, uint d) { c->dreq = d; }

void dma_channel_configure(uint channel, dma_channel_config *c, volatile void *write_addr,
                           const volatile void *read_addr, uint transfer_count, bool trigger) {
    if (channel >= DMA_N_CHANNELS) return;
    dma_ch[channel].from_adc = (read_addr == (const volatile void *)&adc_hw->fifo) || (c->dreq == DREQ_ADC);
    dma_ch[channel].dst = (uint16_t *)write_addr;
    dma_ch[channel].count = transfer_count;
    dma_ch[channel].done = 0;
    dma_ch[channel].busy = trigger;
}

bool dma_channel_is_busy(uint channel) {
    return (channel < DMA_N_CHANNELS) && dma_ch[channel].busy;
}

void dma_channel_wait_for_finish_blocking(uint channel) {
    if (channel >= DMA_N_CHANNELS) return;
    /* Let time carry the transfer to completion, a sample interval at a
     * time, so the plant keeps evolving under the capture. Bounded so a
     * capture that can never complete -- the ADC never started -- fails
     * as a short run of zeros rather than as a hang. */
    uint64_t guard = 0;
    while (dma_ch[channel].busy && guard < 10ull * 1000ull * 1000ull) {
        shim_advance_us(1);
        guard++;
    }
    dma_ch[channel].busy = false;
}

/* Called from inside the time advance: the capture runs whether or not the
 * CPU is looking at it, which is what puts the pre-trigger baseline in the
 * buffer while the firmware is still in busy_wait_us(). */
static void dma_service(double us) {
    if (!adc_running || !adc_fifo_en)
        return;
    adc_accum_us += us;
    while (adc_accum_us >= adc_sample_interval_us) {
        adc_accum_us -= adc_sample_interval_us;
        uint16_t sample = plant_adc_counts((int)adc_input);
        for (int i = 0; i < DMA_N_CHANNELS; i++) {
            if (!dma_ch[i].busy || !dma_ch[i].from_adc || !dma_ch[i].dst)
                continue;
            if (dma_ch[i].done < dma_ch[i].count)
                dma_ch[i].dst[dma_ch[i].done++] = sample;
            if (dma_ch[i].done >= dma_ch[i].count)
                dma_ch[i].busy = false;
        }
    }
}

/* ── PIO ─────────────────────────────────────────────────────────── */

/* arm_pump.pio is 50 PIO clocks per toggle cycle: 25 high, 25 low. */
#define ARM_PUMP_CLOCKS_PER_CYCLE 50.0
#define PIO_FIFO_DEPTH 4

struct pio_instance { int id; };
static struct pio_instance pio0_storage = {0};
static struct pio_instance pio1_storage = {1};
PIO const pio0 = &pio0_storage;
PIO const pio1 = &pio1_storage;

static struct {
    bool     enabled;
    uint     pin;
    double   half_period_us;
    double   to_next_edge_us;
    bool     level;
    uint32_t cycles_left;   /* toggle cycles left in the current burst */
    uint32_t fifo[PIO_FIFO_DEPTH];
    int      fifo_count;
} pump;

static pio_sm_config pending_cfg;

uint pio_add_program(PIO pio, const pio_program_t *program) { (void)pio; (void)program; return 0; }
int  pio_claim_unused_sm(PIO pio, bool required) { (void)pio; (void)required; return 0; }
void pio_gpio_init(PIO pio, uint pin) { (void)pio; pump.pin = pin; }
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint b, uint n, bool o) {
    (void)pio; (void)sm; (void)b; (void)n; (void)o;
}
void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count) {
    c->pin_base = set_base; c->pin_count = set_count;
}
void sm_config_set_clkdiv(pio_sm_config *c, float div) { c->clkdiv = div; }

void pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config) {
    (void)pio; (void)sm; (void)initial_pc;
    pending_cfg = *config;
    pump.pin = config->pin_base;
    /* 50 PIO clocks per cycle at (125 MHz / clkdiv). */
    double cycle_us = ARM_PUMP_CLOCKS_PER_CYCLE * (double)config->clkdiv / 125.0;
    pump.half_period_us = cycle_us / 2.0;
    if (pump.half_period_us <= 0.0)
        pump.half_period_us = 50.0;
    pump.to_next_edge_us = pump.half_period_us;
    pump.level = false;
    pump.cycles_left = 0;
    pump.fifo_count = 0;
}

void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    (void)pio; (void)sm;
    pump.enabled = enabled;
    if (!enabled) {
        pump.cycles_left = 0;
        pump.fifo_count = 0;
        if (pump.level) {
            pump.level = false;
            plant_set_gpio((int)pump.pin, false);
        }
    }
}

void pio_sm_clear_fifos(PIO pio, uint sm) { (void)pio; (void)sm; pump.fifo_count = 0; }
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm) { (void)pio; (void)sm; return pump.fifo_count >= PIO_FIFO_DEPTH; }

void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data) {
    (void)pio; (void)sm;
    /* Blocks only while the FIFO is full, which is what bounds how far
     * ahead of the firmware's safety checks the pump can ever run. */
    uint64_t guard = 0;
    while (pump.fifo_count >= PIO_FIFO_DEPTH && guard < 1000000) {
        shim_advance_us(1);
        guard++;
    }
    if (pump.fifo_count < PIO_FIFO_DEPTH)
        pump.fifo[pump.fifo_count++] = data;
}

/* Driven from the time advance. When the FIFO empties the SM stalls at
 * `pull block` with the pin low, which is the disarm. */
static void pump_service(double us) {
    if (!pump.enabled)
        return;
    while (us > 0.0) {
        if (pump.cycles_left == 0) {
            if (pump.fifo_count == 0)
                return;                        /* stalled on an empty FIFO */
            uint32_t word = pump.fifo[0];
            memmove(&pump.fifo[0], &pump.fifo[1], sizeof(pump.fifo[0]) * (size_t)(pump.fifo_count - 1));
            pump.fifo_count--;
            pump.cycles_left = word + 1;       /* `jmp x--` runs word+1 times */
            pump.to_next_edge_us = 0.0;        /* first edge immediately */
        }
        if (us < pump.to_next_edge_us) {
            pump.to_next_edge_us -= us;
            return;
        }
        us -= pump.to_next_edge_us;
        pump.to_next_edge_us = pump.half_period_us;
        pump.level = !pump.level;
        plant_set_gpio((int)pump.pin, pump.level);
        if (!pump.level && pump.cycles_left > 0)
            pump.cycles_left--;                /* a cycle is high then low */
    }
}

/* ── Watchdog ────────────────────────────────────────────────────── */

static watchdog_hw_t wd_storage;
watchdog_hw_t *const watchdog_hw = &wd_storage;
static uint64_t wd_deadline_us;
static bool wd_expired;

void hw_clear_bits(volatile uint32_t *addr, uint32_t bits) { *addr &= ~bits; }
void hw_set_bits(volatile uint32_t *addr, uint32_t bits) { *addr |= bits; }

void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) {
    (void)pause_on_debug;
    wd_storage.ctrl |= WATCHDOG_CTRL_ENABLE_BITS;
    wd_storage.load = delay_ms * 1000u;
    wd_deadline_us = now_us + (uint64_t)delay_ms * 1000u;
}
void watchdog_update(void) {
    if (wd_storage.ctrl & WATCHDOG_CTRL_ENABLE_BITS)
        wd_deadline_us = now_us + wd_storage.load;
}
bool watchdog_caused_reboot(void) { return false; }
bool shim_watchdog_expired(void) { return wd_expired; }

static void watchdog_service(void) {
    if ((wd_storage.ctrl & WATCHDOG_CTRL_ENABLE_BITS) && now_us > wd_deadline_us)
        wd_expired = true;
}

/* ── GPIO ────────────────────────────────────────────────────────── */

void gpio_init(uint gpio) {
    /* Pad reset state: input, and whatever the plant already holds. The
     * board files call gpio_init() then set the level then the direction,
     * which is the order that matters and which the plant records. */
    plant_set_gpio_dir((int)gpio, false);
}
void gpio_set_dir(uint gpio, bool out) { plant_set_gpio_dir((int)gpio, out); }
void gpio_put(uint gpio, bool value) { plant_set_gpio((int)gpio, value); }
bool gpio_get(uint gpio) { return plant_get_gpio((int)gpio); }
void gpio_pull_up(uint gpio) { (void)gpio; }
void gpio_pull_down(uint gpio) { (void)gpio; }
void gpio_disable_pulls(uint gpio) { (void)gpio; }
void gpio_set_function(uint gpio, enum gpio_function fn) { (void)gpio; (void)fn; }

/* ── Time ────────────────────────────────────────────────────────── */

/* How long until a background peripheral next needs attention. The clock
 * may jump straight to it: plant_step() subdivides internally to whatever
 * resolution the board model asks for, so the only thing the shim has to
 * land on exactly is a peripheral edge.
 *
 * Without this the shim stepped 1 us at a time and a two-minute flight
 * took minutes of wall clock, almost all of it solving an unchanged
 * network while the board sat idle on the pad. */
static uint64_t next_peripheral_event_us(void) {
    uint64_t next = UINT64_MAX;

    if (pump.enabled && (pump.cycles_left > 0 || pump.fifo_count > 0)) {
        uint64_t e = (uint64_t)pump.to_next_edge_us;
        next = (e < 1) ? 1 : e;
    }
    if (adc_running && adc_fifo_en) {
        double owed = adc_sample_interval_us - adc_accum_us;
        uint64_t e = (owed <= 1.0) ? 1 : (uint64_t)owed;
        if (e < next)
            next = e;
    }
    return next;
}

void shim_advance_us(uint64_t us) {
    while (us > 0) {
        uint64_t h = next_peripheral_event_us();
        if (h > us)
            h = us;
        if (h == 0)
            h = 1;
        us -= h;
        now_us += h;
        pump_service((double)h);
        plant_step((double)h * 1e-6);
        dma_service((double)h);
        watchdog_service();
    }
}

void shim_tick(uint32_t ms) {
    (void)ms;
    shim_advance_us(1000);
}

void shim_advance_to_ms(uint32_t ms) {
    uint64_t target = (uint64_t)ms * 1000u;
    if (target > now_us)
        shim_advance_us(target - now_us);
}

void shim_reset(void) {
    now_us = 0;
    memset(dma_ch, 0, sizeof(dma_ch));
    memset(&pump, 0, sizeof(pump));
    memset(&wd_storage, 0, sizeof(wd_storage));
    wd_deadline_us = 0;
    wd_expired = false;
    adc_init();
}
