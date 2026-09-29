/*
 * Board-independent HAL implementation for RP2040 targets.
 *
 * Implements hal.h in terms of src/board_if.h. Contains no pin numbers and
 * no board names; everything board-specific lives in boards/<name>/.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "flight_events.h"
#include "flash_window.h"
#include "board_id.h"
#include "config.h"
#include "async_task.h"
#include "ms5607_driver.h"
#include "board_if.h"   /* the contract every boards/<name>/ implements */
#include "board_pins.h" /* board-supplied capability macros */
#if BOARD_HAS_BMP280
#include "bmp280_driver.h"
#endif
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "board_identity.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include <lfs.h>
#include <pico_fota_bootloader/core.h>
#include "pressure_sensor.h"
#include "pressure_processing.h"
#include "flight_states.h"
#include "pyro.h"
#include "pyro_release.h"
#include "http_server.h"
#include "flight_log.h"
#include "ground_test_switch.h"
#include "log_plan.h"
#include "pressure_trace.h"
#include "loop_period.h"
#include <math.h>
#include <string.h>

/* ── External dependencies ────────────────────────────────────────── */

extern const struct lfs_config lfs_pico_flash_config;
extern const struct lfs_file_config lfs_pico_file_config;

/* Network (net_glue.c / http_server.c) */
void net_init(void);
void net_start(void);
void net_mdns_poll(void);
void net_mac_init(void);
void net_service(void);

/* ── Hardware-internal pressure types ─────────────────────────────── */
/* These are implementation details of the hardware HAL, not exposed
 * in hal.h.  Flight software reads altitude via pp_read(). */

typedef struct {
    float pressure_pa;
    float temperature_c;
} hal_pressure_t;

#define HAL_PRESSURE_BATCH_SIZE 5

typedef struct {
    struct {
        float pressure_pa;
        float temperature_c;
    } samples[HAL_PRESSURE_BATCH_SIZE];
    uint32_t timestamps_ms[HAL_PRESSURE_BATCH_SIZE];
    int count;
} hal_pressure_batch_t;

/* ── Async task runner ────────────────────────────────────────────── */

#define HW_MAX_TASKS 8
static async_task_t *hw_tasks[HW_MAX_TASKS];
static int hw_task_count = 0;

static void hw_task_register(async_task_t *task) {
    if (hw_task_count < HW_MAX_TASKS)
        hw_tasks[hw_task_count++] = task;
}

/* Run all tasks whose deadline has arrived. */
void hal_tasks_tick(uint32_t now_ms) {
    for (int i = 0; i < hw_task_count; i++) {
        async_task_t *t = hw_tasks[i];
        if (t->tick && (int32_t)(now_ms - t->next_due_ms) >= 0)
            t->tick(t, now_ms);
    }
}

/* ── The ground test switch [GND-TEST-12] ──────────────────────────
 *
 * Read once a loop, as a task, so however often the flight code asks it gets
 * one reading a loop; across two pads the drive and the pull then change, and
 * settle for a loop before the next read. */
static struct {
    async_task_t base; /* MUST be first */
    gt_switch_t sw;
    uint8_t pin, drive;
    bool on;
} gts;

static void gts_tick(async_task_t *base, uint32_t now_ms) {
    base->next_due_ms = now_ms;
    gt_switch_out_t o = gt_switch_step(&gts.sw, gpio_get(gts.pin));
    if (gts.sw.pair) {
        gpio_put(gts.drive, o.drive_high);
        gpio_set_pulls(gts.pin, o.pull_up, !o.pull_up);
    }
}

static void hw_task_register(async_task_t *task);

void hal_ground_test_configure(uint8_t wiring, uint8_t pin, uint8_t drive_pin) {
    if (wiring == GT_WIRING_NONE || pin >= NUM_BANK0_GPIOS)
        return;
    bool pair = wiring == GT_WIRING_PAIR && drive_pin < NUM_BANK0_GPIOS;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_pulls(pin, true, false);
    if (pair) {
        gpio_init(drive_pin);
        gpio_set_dir(drive_pin, GPIO_OUT);
        gpio_put(drive_pin, false);
    }
    gt_switch_begin(&gts.sw, pair);
    gts.pin = pin;
    gts.drive = drive_pin;
    gts.on = true;
    gts.base.tick = gts_tick;
    gts.base.next_due_ms = hal_time_ms();
    hw_task_register(&gts.base);
}

bool hal_ground_test_asserted(void) {
    return gts.on && gt_switch_asserted(&gts.sw);
}

/* Return the earliest next_due_ms across all registered tasks.
 * Returns 0 if no tasks are registered. */
static uint32_t hw_tasks_next_due(void) {
    if (hw_task_count == 0)
        return 0;
    uint32_t min = hw_tasks[0]->next_due_ms;
    for (int i = 1; i < hw_task_count; i++)
        if ((int32_t)(hw_tasks[i]->next_due_ms - min) < 0)
            min = hw_tasks[i]->next_due_ms;
    return min;
}

/* Forward declarations for internal functions */
static bool hal_pressure_fifo_start(void);

/* ── Pressure sensor async state machine [v2 Task 4] ─────────────── */

/*
 * Pressure async task.  struct async_task MUST be the first member so
 * a pointer cast between pres_task_t * and async_task_t * is safe.
 */
typedef struct {
    async_task_t base;    /* MUST be first */
    int sensor_type;      /* 1=MS5607  2=BMP280 */
    ms5607_temps_t temps; /* MS5607: the temperature, carried to each pressure [DD-051] */

    /* Ping-pong batch buffers.  back is filled by the tick function;
     * front is promoted atomically when full and read by the consumer. */
    hal_pressure_batch_t back;
    hal_pressure_batch_t front;
    volatile bool front_ready; /* true when front is valid for consumer */

    /* Latest sample for hal_pressure_read() bridge (transparent mode). */
    hal_pressure_t last;
    bool has_last;

    uint64_t last_stamp_us; /* the previous sample's time, for the interval */
    uint32_t interval_min_us, interval_max_us, stamp_lag_max_us;
    uint32_t waits;   /* loops that found the last conversion still running */
    uint32_t rejects; /* readings no atmosphere can produce, not fed on */
    uint32_t flashed; /* [DD-068] readings a flash operation disturbed, not fed on */
} pres_task_t;

static pres_task_t pres;

uint32_t hal_pressure_waits(void) {
    return pres.waits;
}

uint32_t hal_pressure_rejects(void) {
    return pres.rejects;
}
uint32_t hal_pressure_flashed(void) {
    return pres.flashed;
}

/* [SNS-PRES-08] The spread of the intervals between samples, and the longest
 * any sample waited between its conversion and the loop reading it. */
uint32_t hal_pressure_interval_min_us(void) {
    return pres.interval_min_us;
}

/* Pressure samples a second, and so the full log rate [DD-062]: either
 * sensor converts once a loop [DD-066, DD-067]. 0 before bring-up. */
uint32_t hal_pressure_rate_hz(void) {
    return pres.sensor_type ? 1000u / LOOP_PERIOD_MS : 0u;
}
uint32_t hal_pressure_interval_max_us(void) {
    return pres.interval_max_us;
}
uint32_t hal_pressure_stamp_lag_max_us(void) {
    return pres.stamp_lag_max_us;
}

/* The sensor's own range, 10-1200 mbar. Inside it nothing is judged: a real
 * reading can be anywhere a rocket can go. Outside it the reading is not the
 * atmosphere, and one of them through the IIR is hundreds of metres of
 * altitude -- a launch on the pad, or a trigger in flight. */
#define PRES_MIN_PA 1000.0f
#define PRES_MAX_PA 120000.0f

static bool pres_plausible(const pressure_reading_t *r) {
    return r->pressure_pa >= PRES_MIN_PA && r->pressure_pa <= PRES_MAX_PA;
}

/* Append a completed reading to the batch, update the bridge sample,
 * and feed the pressure_processing pipeline (IIR + altitude ring).
 *
 * [SNS-PRES-08] The reading's time is the driver's: never the loop's, which a
 * flash stall between the conversion and the read would make late by the
 * whole stall. */
static void pres_append(pres_task_t *p, const pressure_reading_t *r) {
    uint64_t stamp_us = r->time_us;
    uint32_t now_ms = (uint32_t)(stamp_us / 1000u);
    p->last.pressure_pa = r->pressure_pa;
    p->last.temperature_c = r->temperature_c;
    p->has_last = true;

    uint64_t read_us = time_us_64();
    uint32_t lag = (uint32_t)(read_us - stamp_us);
    if (lag > p->stamp_lag_max_us)
        p->stamp_lag_max_us = lag;
    if (p->last_stamp_us != 0) {
        uint32_t iv = (uint32_t)(stamp_us - p->last_stamp_us);
        if (p->interval_min_us == 0 || iv < p->interval_min_us)
            p->interval_min_us = iv;
        if (iv > p->interval_max_us)
            p->interval_max_us = iv;
    }
    p->last_stamp_us = stamp_us;

    /* Feed the pressure_processing ring so detectors can read altitude
     * samples via pp_read() — single data path from sensor to FSM. */
    pp_feed_us((int32_t)r->pressure_pa, stamp_us);

    int idx = p->back.count;
    if (idx < HAL_PRESSURE_BATCH_SIZE) {
        p->back.samples[idx].pressure_pa = r->pressure_pa;
        p->back.samples[idx].temperature_c = r->temperature_c;
        p->back.timestamps_ms[idx] = now_ms;
        p->back.count++;
    }

    /* Promote back → front when the batch is full and the consumer has
     * released the previous front.  If the consumer is slow we keep
     * overwriting back; the flight filter handles repeated readings. */
    if (p->back.count >= HAL_PRESSURE_BATCH_SIZE && !p->front_ready) {
        p->front = p->back;
        p->front_ready = true;
        p->back.count = 0;
    }

    /* Proof-of-life heartbeat: toggle LED every HAL_PRESSURE_BATCH_SIZE
     * samples (50 Hz / 5 = 10 Hz toggle = ~5 Hz visible blink). */
    static uint8_t led_n = 0;
    if (++led_n >= HAL_PRESSURE_BATCH_SIZE) {
        led_n = 0;
        board_led_toggle();
    }
}

static void pres_reject(pres_task_t *p, const char *why, uint32_t raw, float pa, uint32_t now_ms) {
    p->rejects++;
    char dbuf[80];
    snprintf(dbuf, sizeof(dbuf), "!PRES %s raw=%lu pa=%.0f t=%lu\r\n", why, (unsigned long)raw, (double)pa,
             (unsigned long)now_ms);
    hal_telemetry_send(dbuf);
}

/* [DD-051, DD-066] The pair the last loop started, which the one-shot's
 * handler commanded, stamped and read. The cycle starts the next pair before
 * this work: done first, the compensation, filter and fit (up to 2.7 ms on
 * MK1B) made the next conversion miss the next loop every other loop. */
static void ms5607_tick(pres_task_t *p, uint32_t now_ms) {
    ms5607_pair_t c;
    ms5607_start_t started;
    bool took = ms5607_async_cycle(&p->temps, &c, &started);
    uint32_t read_us = (uint32_t)time_us_64();
    if (started == MS5607_BUSY) {
        p->waits++;
        ptrace_note(read_us, read_us, 0, 0, 0, PTRACE_MISSED);
    }
    /* HELD: the sensor did not answer; back off rather than retry every loop. */
    p->base.next_due_ms = (started == MS5607_STARTED || started == MS5607_BUSY) ? now_ms : now_ms + 50;
    if (!took)
        return;
    if (!c.ok) {
        ptrace_note((uint32_t)c.d1_at_us, read_us, c.d1, 0, 0, PTRACE_BUS);
        pres_reject(p, "bus", c.d1, 0.0f, now_ms);
        return;
    }
    /* A zero is what the sensor answers to a read during a conversion; the
     * cycle notes neither a zero temperature nor a disturbed one. */
    ptrace_note((uint32_t)c.d2_at_us, read_us, c.d2, 0, 0,
                c.d2 == 0 ? PTRACE_ZERO : (c.d2_flashed ? PTRACE_FLASHED_T : PTRACE_TEMPERATURE));
    if (c.d2 == 0)
        pres_reject(p, "zero", 0, 0.0f, now_ms);
    else if (c.d2_flashed)
        p->flashed++;
    if (c.d1 == 0) {
        ptrace_note((uint32_t)c.d1_at_us, read_us, 0, 0, 0, PTRACE_ZERO);
        pres_reject(p, "zero", 0, 0.0f, now_ms);
        return;
    }
    if (p->temps.n == 0)
        return;
    pressure_reading_t r;
    ms5607_compensate(c.d1, ms5607_temps_at(&p->temps, c.d1_at_us), &r);
    r.time_us = c.d1_at_us;
    int32_t pa_c = (int32_t)lroundf(r.pressure_pa * 100.0f);
    if (c.d1_flashed) {
        ptrace_note((uint32_t)c.d1_at_us, read_us, c.d1, 0, pa_c, PTRACE_FLASHED);
        p->flashed++;
        return;
    }
    if (!pres_plausible(&r)) {
        ptrace_note((uint32_t)c.d1_at_us, read_us, c.d1, 0, pa_c, PTRACE_RANGE);
        pres_reject(p, "range", c.d1, r.pressure_pa, now_ms);
        return;
    }
    ptrace_note((uint32_t)c.d1_at_us, read_us, c.d1, 0, pa_c, PTRACE_PRESSURE);
    pres_append(p, &r);
}

/*
 * Pressure tick, once a loop.
 *
 * MS5607 (sensor_type == 1) [DD-051, DD-066]: take the pair the last loop
 *   started and start the next, then work on the one taken; the one-shot's
 *   handler commands, stamps and reads each. A pressure and a temperature
 *   every loop: 50 pressures a second at the 20 ms loop.
 *
 * BMP280 (sensor_type == 2) [DD-067]: take the forced conversion the last
 *   loop commanded and command the next. 50 pressures a second.
 */
static void pres_tick(async_task_t *base, uint32_t now_ms) {
    pres_task_t *p = (pres_task_t *)base;

    if (p->sensor_type == 1) {
        ms5607_tick(p, now_ms);

#if BOARD_HAS_BMP280
    } else if (p->sensor_type == 2) {
        /* Only reachable on boards that declare BOARD_HAS_BMP280; elsewhere
         * pressure_sensor_step() can never report type 2. */
        bmp280_reading_t r;
        bmp280_start_t started;
        uint32_t read_us = (uint32_t)time_us_64();
        bool took = bmp280_cycle(&r, &started);
        if (started == BMP280_BUSY) {
            p->waits++;
            ptrace_note(read_us, read_us, 0, 0, 0, PTRACE_MISSED);
        } else if (started == BMP280_BUS) {
            ptrace_note(read_us, read_us, 0, 0, 0, PTRACE_BUS);
        }
        if (took) {
            bool ok = pres_plausible(&r.reading);
            ptrace_note((uint32_t)r.reading.time_us, read_us, r.adc_p, r.adc_t,
                        (int32_t)lroundf(r.reading.pressure_pa * 100.0f),
                        r.flashed ? PTRACE_FLASHED : (ok ? PTRACE_PRESSURE : PTRACE_RANGE));
            if (r.flashed)
                p->flashed++;
            else if (ok)
                pres_append(p, &r.reading);
            else
                p->rejects++;
        }
        p->base.next_due_ms = now_ms;
#endif
    }
}

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* The RP2040 records what reset it: a POR (which a brownout also drives), the
 * RUN pin, a debugger's PSM restart. The watchdog is asked first because the
 * OTA path and the web UI's reboot button both go through watchdog_reboot(),
 * and those must not look like power events -- a deliberate reboot on the pad
 * should recalibrate, not go hunting for a flight to rejoin. */
reset_cause_t hal_reset_cause(void) {
    if (watchdog_caused_reboot()) {
        return RESET_SOFTWARE;
    }
    uint32_t r = vreg_and_chip_reset_hw->chip_reset;
    if (r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) {
        return RESET_RUN_PIN;
    }
    if (r & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) {
        return RESET_DEBUG;
    }
    /* HAD_POR, or none of them: treat an unreadable cause as a power event,
     * which is the answer that keeps the recovery path available. */
    return RESET_POWER_EVENT;
}

/* ── Pressure sensor ──────────────────────────────────────────────── */

/* -1 while the sensor is being brought up. */
static int hw_sensor_type = -1;

/* [DD-053] The bring-up, a step a loop, on the pressure task's own slot. Once
 * it knows the sensor, the same slot samples it: the BMP280 at 50 Hz, the
 * MS5607 every loop. */
static void pres_bringup_tick(async_task_t *base, uint32_t now_ms) {
    base->next_due_ms = now_ms;
    pressure_sensor_type_t t = pressure_sensor_step(now_ms);
    if (t == PRESSURE_SENSOR_PENDING)
        return;
    hw_sensor_type = (int)t;
    if (t == PRESSURE_SENSOR_NONE || !hal_pressure_fifo_start())
        pres.base.tick = NULL;
}

void hal_pressure_init(void) {
    hw_sensor_type = -1;
    pressure_sensor_begin();
    memset(&pres, 0, sizeof(pres));
    pres.base.tick = pres_bringup_tick;
    pres.base.next_due_ms = hal_time_ms();
    hw_task_register(&pres.base);
}

int hal_pressure_sensor(void) {
    return hw_sensor_type;
}

/* Returns the most recent async sample.  In the v2 architecture all
 * pressure reads come from the async task — there is no synchronous
 * fallback.  Returns false when the async task hasn't produced a
 * sample yet (the MS5607's first pressure waits on its first temperature). */
bool hal_pressure_read(hal_pressure_t *out) {
    if (pres.has_last) {
        *out = pres.last;
        return true;
    }
    return false;
}

/* ── Pressure FIFO (v2 async batch API) ───────────────────────────── */

static bool hal_pressure_fifo_start(void) {
    if (hw_sensor_type <= 0)
        return false;
    memset(&pres, 0, sizeof(pres));
    pres.base.tick = pres_tick;
    pres.base.next_due_ms = hal_time_ms(); /* run on first tick */
    pres.sensor_type = hw_sensor_type;
    if (pres.sensor_type == 1 && !ms5607_async_begin()) {
        hal_telemetry_send("!PRES no hardware alarm free for the MS5607\r\n");
        return false;
    }
    /* Register once (idempotent — re-calling changes rate but not slot). */
    for (int i = 0; i < hw_task_count; i++)
        if (hw_tasks[i] == &pres.base)
            return true;
    hw_task_register(&pres.base);
    return true;
}

bool hal_pressure_fifo_get(hal_pressure_batch_t *batch) {
    if (!pres.front_ready)
        return false;
    *batch = pres.front;
    return true;
}

void hal_pressure_fifo_release(void) {
    pres.front_ready = false;
}

bool hal_pressure_fifo_active(void) {
    return pres.base.tick == pres_tick;
}

/* ── Pyro ─────────────────────────────────────────────────────────── */

/* ── The real channel operations ──────────────────────────────────
 *
 * Installed per channel by pyro_release_claim(), and only for a channel whose
 * pads it could claim. A channel Lua already holds gets the mocked table, and
 * these are then simply not reachable for it. See pad_claim.h.
 *
 * Not named *_vt on purpose. That suffix means "core1 can reach this" and
 * prove_core0.py folds those into the core1 proof; these run on core0 only. */
static void real_fire(uint8_t channel) {
    pyro_fire(channel);
}

static void real_get(uint8_t channel, hal_continuity_t *out) {
    pyro_continuity_t c;
    pyro_get(channel, &c);
    out->raw_adc = c.raw_adc;
    out->good = c.good;
    out->open = c.open;
    out->shorted = c.shorted;
}

static bool real_fault(uint8_t channel) {
    return pyro_fault(channel);
}

static const pyro_ch_ops_t real_pyro_ops = {real_fire, real_get, real_fault};

/* Said in three places, because each reaches a different audience: the flight
 * log during the flight, the telemetry downlink at the time, and the counter
 * on /api/status afterwards. The log row is the one that matters -- a flight
 * log showing PYRO1 with nothing beside it is a record of an ignition that
 * did not happen. */
static void report_mock(uint8_t channel, const char *what) {
    char note[48];
    snprintf(note, sizeof(note), "pyro%u %s: released to Lua", (unsigned)channel, what);
    /* [DAT-02, N11] On the flight log's clock, since T+0, like every other row. */
    const flight_context_t *fc = flight_get_context();
    uint32_t now = to_ms_since_boot(get_absolute_time());
    hal_log_mock(fc ? flight_elapsed_ms(fc, now) : now, note);

    char line[64];
    snprintf(line, sizeof(line), "!MOCK %s\r\n", note);
    hal_telemetry_send(line);
}

void hal_pyro_init(void) {
    pyro_release_init(&real_pyro_ops, report_mock);
    pyro_init();
}

int hal_pyro_claim_channels(uint32_t (*pads_of)(uint8_t channel)) {
    return pyro_release_claim(pads_of);
}

void hal_pyro_sample(void) {
    /* The continuity stimulus drives the COMMON element, which is Lua's
     * exactly when both channels are released. Sampling then would put core0
     * and core1 on the same pad, and there would be nothing left to measure
     * anyway. */
    if (pyro_release_all()) {
        return;
    }
    pyro_sample();
}

void hal_pyro_get(uint8_t channel, hal_continuity_t *out) {
    pyro_ch(channel)->get(channel, out);
}

void hal_pyro_fire(uint8_t channel) {
    pyro_ch(channel)->fire(channel);
}
void hal_pyro_update(uint32_t now_ms) {
    /* Same argument as hal_pyro_sample(): MK1A's background sense cycle and
     * MK1C's arm pump both drive the common, and with both channels released
     * there is nothing to arm and nothing to sense. */
    if (pyro_release_all()) {
        return;
    }
    pyro_update(now_ms);
}
bool hal_pyro_is_firing(void) {
    return pyro_is_firing();
}
bool hal_pyro_fault(uint8_t channel) {
    return pyro_ch(channel)->fault(channel);
}

/* ── Buzzer ───────────────────────────────────────────────────────── */

void hal_buzzer_init(void) {
    board_buzzer_init();
}

void hal_buzzer_tone_on(void) {
    board_buzzer_on();
}
void hal_buzzer_tone_off(void) {
    board_buzzer_off();
}

/* Register the buzzer async task with the hardware task runner.
 * buzzer_init() calls this so the buzzer task runs alongside the
 * pressure task without any main-loop involvement. */
void hal_buzzer_task_register(async_task_t *task) {
    hw_task_register(task);
}

/* ── UART TX ring buffer (v2-10) ──────────────────────────────────── */
/*
 * ISR-driven UART0 TX.  hal_telemetry_send() copies bytes into a 512-byte
 * circular buffer and re-arms the UART0 TX interrupt.  The ISR drains the
 * ring into the PL011 TX FIFO on each TX-FIFO-half-empty event.
 *
 * Telemetry is best-effort: if the ring is full, remaining bytes are dropped.
 *
 * RX is unchanged: hal_serial_readline() polls the RX FIFO directly.
 */

#define UART_TX_BUF_SIZE 512
#define UART_TX_BUF_MASK (UART_TX_BUF_SIZE - 1)

/* Telemetry UART instance, supplied by the board. Cached on first use so
 * the TX ISR does not make a cross-module call on every byte. */
static uart_inst_t *s_uart;
static inline uart_inst_t *tuart(void) {
    if (!s_uart)
        s_uart = board_uart();
    return s_uart;
}

static volatile uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static volatile int uart_tx_head = 0;
static volatile int uart_tx_tail = 0;

static void uart_tx_drain_isr(void) {
    while (uart_is_writable(tuart())) {
        int t = uart_tx_tail;
        if (t == uart_tx_head)
            break;
        uart_get_hw(tuart())->dr = uart_tx_buf[t];
        uart_tx_tail = (t + 1) & UART_TX_BUF_MASK;
    }
    if (uart_tx_tail == uart_tx_head)
        hw_clear_bits(&uart_get_hw(tuart())->imsc, UART_UARTIMSC_TXIM_BITS);
}

static void telem_uart_irq_handler(void) {
    if (uart_get_hw(tuart())->mis & UART_UARTMIS_TXMIS_BITS)
        uart_tx_drain_isr();
}

static void uart_tx_ring_init(void) {
    irq_set_exclusive_handler(board_uart_irq(), telem_uart_irq_handler);
    irq_set_enabled(board_uart_irq(), true);
    hw_clear_bits(&uart_get_hw(tuart())->imsc, UART_UARTIMSC_TXIM_BITS);
}

/* ── Telemetry ────────────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence) {
    if (!sentence)
        return;

    /* Check if ring was empty before adding new data */
    bool was_empty = (uart_tx_head == uart_tx_tail);

    for (const char *p = sentence; *p; p++) {
        int next = (uart_tx_head + 1) & UART_TX_BUF_MASK;
        if (next == uart_tx_tail)
            break; /* ring full — drop */
        uart_tx_buf[uart_tx_head] = (uint8_t)*p;
        uart_tx_head = next;
    }

    /* If ring was empty, manually prime the UART FIFO to trigger first interrupt */
    if (was_empty && uart_tx_head != uart_tx_tail) {
        while (uart_is_writable(tuart()) && uart_tx_tail != uart_tx_head) {
            uart_get_hw(tuart())->dr = uart_tx_buf[uart_tx_tail];
            uart_tx_tail = (uart_tx_tail + 1) & UART_TX_BUF_MASK;
        }
    }

    /* Enable TX interrupt to continue draining ring buffer */
    if (uart_tx_head != uart_tx_tail)
        hw_set_bits(&uart_get_hw(tuart())->imsc, UART_UARTIMSC_TXIM_BITS);
}

/* ── Filesystem ───────────────────────────────────────────────────── */

/* Refusing partway through an lfs operation leaves its metadata half
 * written, so this stops a multi-block operation from starting at all. The
 * driver's own check is the backstop. A refusal is counted like the driver's:
 * a caller outside the window is a bug, and uncounted it fails silently. */
static bool flash_writable(void) {
    if (flash_window_is_open())
        return true;
    flash_window_refused();
    return false;
}

static bool fs_ok;

/* [WEB-API-08, DD-058] Mounts other than the flight log's, open now. The log
 * waits for none before it mounts, and none may start while it holds. */
static int fs_borrowed;

bool hal_fs_locked(void) {
    return hal_log_active();
}

int hal_fs_enter(void) {
    if (hal_fs_locked())
        return HAL_FS_LOCKED;
    fs_borrowed++;
    return 0;
}

void hal_fs_leave(void) {
    if (fs_borrowed > 0)
        fs_borrowed--;
}

bool hal_fs_healthy(void) {
    return fs_ok;
}

int hal_fs_mount(void) {
    lfs_t lfs;
    int err = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&lfs, &lfs_pico_flash_config);
        err = lfs_mount(&lfs, &lfs_pico_flash_config);
    }
    if (err == 0)
        lfs_unmount(&lfs);
    fs_ok = (err == 0);
    return err;
}

void hal_fs_unmount(void) {
    /* Each read/write mounts and unmounts internally */
}

static int read_file(const char *path, char *buf, int max_len) {
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return -1;
    lfs_file_t f;
    int err = lfs_file_opencfg(&lfs, &f, path, LFS_O_RDONLY, &lfs_pico_file_config);
    if (err == LFS_ERR_NOENT) {
        lfs_unmount(&lfs);
        return -2;
    }
    if (err != LFS_ERR_OK) {
        lfs_unmount(&lfs);
        return -1;
    }
    lfs_ssize_t n = lfs_file_read(&lfs, &f, buf, max_len);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    return (int)n;
}

int hal_fs_read_file(const char *path, char *buf, int max_len) {
    int err = hal_fs_enter();
    if (err != 0)
        return err;
    int n = read_file(path, buf, max_len);
    hal_fs_leave();
    return n;
}

static int write_file(const char *path, const char *data, int len) {
    if (!flash_writable())
        return -1;

    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return -1;
    lfs_file_t f;
    if (lfs_file_opencfg(&lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &lfs_pico_file_config) !=
        LFS_ERR_OK) {
        lfs_unmount(&lfs);
        return -1;
    }
    lfs_file_write(&lfs, &f, data, len);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    return 0;
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    int err = hal_fs_enter();
    if (err != 0)
        return err;
    err = write_file(path, data, len);
    hal_fs_leave();
    return err;
}

/* ── Streaming file writes ─────────────────────────────────────────── */

struct hal_file {
    lfs_t lfs;
    lfs_file_t file;
    bool open;
    bool borrowed; /* not the flight log's: counted by hal_fs_enter() */
};

static struct hal_file hw_file;

static hal_file_t *open_file(const char *path, bool append) {
    /* There is no read mode here: both paths are LFS_O_WRONLY | LFS_O_CREAT,
     * and creating the file commits a dirent. */
    if (!flash_writable())
        return NULL;

    if (hw_file.open)
        return NULL;
    if (lfs_mount(&hw_file.lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return NULL;
    int flags = LFS_O_WRONLY | LFS_O_CREAT;
    flags |= append ? LFS_O_APPEND : LFS_O_TRUNC;
    if (lfs_file_opencfg(&hw_file.lfs, &hw_file.file, path, flags, &lfs_pico_file_config) != LFS_ERR_OK) {
        lfs_unmount(&hw_file.lfs);
        return NULL;
    }
    hw_file.open = true;
    hw_file.borrowed = false;
    return &hw_file;
}

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (hal_fs_enter() != 0)
        return NULL;
    hal_file_t *f = open_file(path, append);
    if (!f) {
        hal_fs_leave();
        return NULL;
    }
    f->borrowed = true;
    return f;
}

int hal_fs_write(hal_file_t *f, const char *data, int len) {
    if (!f || !f->open)
        return -1;
    return (int)lfs_file_write(&f->lfs, &f->file, data, len);
}

void hal_fs_close(hal_file_t *f) {
    if (!f || !f->open)
        return;
    lfs_file_close(&f->lfs, &f->file);
    lfs_unmount(&f->lfs);
    f->open = false;
    if (f->borrowed)
        hal_fs_leave();
}

/* ── Config (v2) ──────────────────────────────────────────────────── */

int hal_config_load(config_t *cfg) {
    config_set_defaults(cfg);
    char buf[512];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        config_parse_ini(buf, cfg);
        return 0;
    }
    /* No config file — write defaults for next boot */
    const char *def = config_default_ini();
    hal_fs_write_file("config.ini", def, (int)strlen(def));
    return -1;
}

int hal_config_save(const config_t *cfg) {
    char buf[512];
    int n = config_serialize_ini(cfg, buf, (int)sizeof(buf));
    if (n <= 0)
        return -1;
    return hal_fs_write_file("config.ini", buf, n);
}

/* ── Serial readline (v2, telemetry UART RX) ──────────────────────── */

bool hal_serial_readline(char *buf, int max_len) {
    static char rx_buf[64];
    static int rx_len = 0;

    while (uart_is_readable(tuart()) && rx_len < (int)(sizeof(rx_buf) - 1)) {
        char c = (char)uart_getc(tuart());
        if (c == '\n' || c == '\r') {
            if (rx_len > 0) {
                int n = (rx_len < max_len - 1) ? rx_len : max_len - 1;
                memcpy(buf, rx_buf, n);
                buf[n] = '\0';
                rx_len = 0;
                return true;
            }
            /* empty line — skip */
        } else {
            rx_buf[rx_len++] = c;
        }
    }
    return false;
}

/* Deliberately a no-op: __wfe() is suspected of blocking USB NCM TX, which
 * shows up as the txf counter climbing and http stuck at zero. */

void hal_sleep_until_event(void) {}

/* ── Platform ─────────────────────────────────────────────────────── */

void hal_platform_init(void) {
    /* Silence buzzer GPIO immediately — before any slow init (board, USB,
     * network) that could leave the pin floating and produce a spurious
     * tone at power-on. */
    hal_buzzer_init();

    /* Puts the pyro outputs down before anything slow runs; without it the
     * firing pins keep their reset state until pyro_init(), which is after
     * USB enumeration, lwIP and the filesystem mount. */
    board_early_init();

    /* Watchdog initialization removed - watchdog_reboot() handles enabling
     * internally when needed. The 1ms timeout was causing boot loops. */

    board_init();

    /* Board half of init: LED, telemetry UART pins, ADC inputs.
     * Must be after the BSP's board_init(), which reinitialises any pin the
     * board header named via PICO_DEFAULT_LED_PIN. */
    board_hw_init();

    /* Before net_mac_init() and tud_init(), both of which consume it: the MAC
     * goes into the ECM descriptor and the subnet into the DHCP server, and
     * the host reads each exactly once, at enumeration.
     *
     * Reads /serial.txt via hal_fs_read_file(), which mounts read-only and
     * does NOT format on failure -- so a board with no filesystem yet falls
     * back to its hardware id and enumerates normally, instead of waiting out
     * an 8 MB format before USB appears. */
    board_identity_init();
    net_mac_init();
    tud_init(BOARD_TUD_RHPORT);
    /* stdio_init_all() removed — we own the telemetry UART exclusively for ISR-driven
     * telemetry TX and ground-test RX.  No SDK stdio drivers needed. */

    uart_init(tuart(), 115200); /* pins were assigned by board_hw_init() */
    uart_tx_ring_init();        /* v2-10: non-blocking TX via ISR ring buffer */

    net_init();
    net_start();
    http_server_init();

    /* Format/mount littlefs once at boot so HTTP file uploads work */
    hal_fs_mount();

    pfb_firmware_commit();

    /* The sensor's I2C is its bring-up's to set up: pressure_sensor_begin()
     * resets the peripheral and recovers the bus itself. */
}

/* STAGE 1's peak, by part: TinyUSB, lwIP, HTTP transport, mDNS. */
volatile uint32_t stage1_part_max_us[4];

static void note_part(int part, uint32_t us) {
    if (us > stage1_part_max_us[part])
        stage1_part_max_us[part] = us;
}

void hal_platform_service(void) {
    extern uint32_t net_last_http_us;
    uint32_t t0 = time_us_32();
    tud_task();
    uint32_t t1 = time_us_32();
    net_service();
    uint32_t t2 = time_us_32();
    net_mdns_poll();
    uint32_t t3 = time_us_32();
    note_part(0, t1 - t0);
    note_part(1, (t2 - t1) - net_last_http_us);
    note_part(2, net_last_http_us);
    note_part(3, t3 - t2);
}

/* ── Pressure sample override [v2-8] ─────────────────────────────── */

void hal_pressure_push_sample(const hal_pressure_t *sample) {
    if (sample) {
        pres.last = *sample;
        pres.has_last = true;
    } else {
        /* clear override — next read comes from the async task as normal */
        pres.has_last = false;
    }
}

/* ── In-flight data logging (REQUIREMENTS.md v2-9) ───────────────
 *
 * Nothing on the flight path reaches flash: not the first sample, not a full
 * buffer, not the file's own creation. hal_flash_service() is the other
 * half, and is the only thing in this file that erases or programs.
 *
 * Do not move it onto the async task list: those run at STAGE 2, before
 * core0 has checked core1's grant and usually while core1 is mid-unit.
 */

/* Binary records (flight_log.h), rendered as CSV only when downloaded
 * [DD-062]. 4 KB holds 1.9 s of samples at 100 Hz, enough to span the launch
 * shock window below; at a row a second it would hold three minutes, so the
 * holdoff also ends on a timer. */
#define LOG_BUF_SIZE 4096
#define LOG_FLUSH_MS 200u
#define LOG_HOLDOFF_MAX_MS 2000u /* [FLT-LOG-05] */

/* Room for the rows of the periods before the next window. */
#define LOG_WATERMARK (LOG_BUF_SIZE - 96)

/* Script text shares this buffer with the flight samples and ranks below
 * them, so it is admitted only in the lower half: a chatty script loses its
 * own lines rather than a single altitude reading. */
#define LOG_TEXT_CEILING (LOG_BUF_SIZE / 2)

/* How much flight a power loss can take with it. littlefs publishes what a
 * file holds only on sync or close, and the log closes at LANDED, so a flight
 * that never lands -- a crash, a battery that lets go on impact -- would
 * leave an empty file. A sync makes the next write copy the file's partial
 * last block, an erase, so both land in the window like every other write. */
#define LOG_SYNC_MS 1000u

typedef struct {
    hal_file_t *file;
    uint8_t buf[LOG_BUF_SIZE];
    int head; /* write cursor */
    bool active;
    bool stopping;
    bool pending_open; /* the file still has to be created, inside a window */
    uint32_t next_due_ms;
    uint32_t next_sync_ms;
    bool unsynced;         /* written since the last sync */
    uint32_t dropped;      /* sample bytes the buffer could not hold */
    uint32_t text_dropped; /* script bytes refused: not a lost sample */
    /* No flash write until the buffer has filled once. Launch shock is the
     * likeliest cause of a brownout -- a battery connector bouncing -- and a
     * flash write in progress is the worst moment to lose power, so the first
     * seconds of the flight live in RAM. Cleared on the first watermark hit
     * or at LOG_HOLDOFF_MAX_MS, after which flushing is periodic: this delays
     * the first write past the shock, it does not make the flight write-once. */
    bool launch_holdoff;
    uint32_t started_ms;
} log_task_t;

static log_task_t log_task;
static log_plan_t log_plan;

static void log_keep(void *ctx, const flog_sample_t *s) {
    (void)ctx;
    /* Do not flush synchronously here: this runs on the flight path, and a
     * flush would put an erase wherever a sample happened to overflow the
     * buffer -- an arbitrary point in the period, with core1 mid-unit. */
    int n = flog_put_sample(log_task.buf + log_task.head, LOG_BUF_SIZE - log_task.head, s);
    if (n == 0) {
        log_task.dropped += FLOG_SAMPLE_BYTES;
        return;
    }
    log_task.head += n;
}

uint32_t hal_log_dropped(void) {
    return log_task.dropped;
}

void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa) {
    if (log_task.active)
        return;

    /* Called at liftoff, so it opens no file and writes no flash: a launch
     * that waited for an erase would be a launch detected late. */
    log_task.dropped = 0;
    flog_header_t h = {
        .board = PYRO_BOARD_NAME,
        .id = cfg->id,
        .name = cfg->name,
        .pyro1_mode = cfg->pyro1_mode,
        .pyro2_mode = cfg->pyro2_mode,
        .pyro1_value = cfg->pyro1_value,
        .pyro2_value = cfg->pyro2_value,
        .units = cfg->units,
        .ground_pa = ground_pressure_pa,
        .rate = cfg->log_rate,
    };
    log_task.head = flog_put_header(log_task.buf, LOG_BUF_SIZE, &h);
    log_plan_init(&log_plan, cfg->log_rate);

    log_task.file = NULL;
    log_task.pending_open = true;
    log_task.active = true;
    log_task.stopping = false;
    log_task.next_due_ms = hal_time_ms() + LOG_FLUSH_MS;
    log_task.next_sync_ms = hal_time_ms();
    log_task.unsynced = false;
    log_task.launch_holdoff = true;
    log_task.started_ms = hal_time_ms();
}

/* Only from log_flash_service(), in the window. On a failure the next window
 * retries: the file keeps its last published length meanwhile. */
static void log_sync_if_due(uint32_t now_ms) {
    hal_file_t *f = log_task.file;
    if (!f || !f->open || !log_task.unsynced || (int32_t)(now_ms - log_task.next_sync_ms) < 0)
        return;
    if (lfs_file_sync(&f->lfs, &f->file) == LFS_ERR_OK)
        log_task.unsynced = false;
    log_task.next_sync_ms = now_ms + LOG_SYNC_MS;
}

/* Every branch here can fail costing only a delay: the next window retries,
 * and the buffer keeps its bytes until a write takes them. Only a buffer
 * that filled faster than the windows drained it loses anything. */
static void log_flash_service(uint32_t now_ms) {
    if (!log_task.active)
        return;

    if (log_task.pending_open) {
        /* A web transfer mounted before launch lets go on its next pass
         * [WEB-API-08]; the rows wait in the buffer meanwhile. */
        if (fs_borrowed > 0)
            return;
        log_task.file = open_file(FLOG_PATH, false);
        if (!log_task.file)
            return; /* window refused or the fs is busy: try the next one */
        log_task.pending_open = false;
    }

    bool full = log_task.head >= LOG_WATERMARK;
    if (full || now_ms - log_task.started_ms >= LOG_HOLDOFF_MAX_MS) {
        log_task.launch_holdoff = false;
    }
    /* The timer does not fire during the holdoff; a full buffer and a stop
     * always do, so nothing is ever dropped to keep the flash quiet. */
    bool due = !log_task.launch_holdoff && (int32_t)(now_ms - log_task.next_due_ms) >= 0;
    if (full || log_task.stopping)
        due = true;
    if (!due) {
        /* In a window with no write in it, so a sync and a block write never
         * share one window's stall. */
        log_sync_if_due(now_ms);
        return;
    }

    if (log_task.head > 0 && log_task.file) {
        int n = hal_fs_write(log_task.file, (const char *)log_task.buf, log_task.head);
        if (n > 0) {
            /* Only what was written leaves: a record written twice would
             * misalign every record after it. */
            memmove(log_task.buf, log_task.buf + n, (size_t)(log_task.head - n));
            log_task.head -= n;
            log_task.unsynced = true;
        }
        /* A failed write keeps the bytes: the next window retries. */
    }
    log_task.next_due_ms = now_ms + LOG_FLUSH_MS;

    if (log_task.stopping && log_task.head == 0) {
        if (log_task.file) {
            hal_fs_close(log_task.file);
            log_task.file = NULL;
        }
        log_task.active = false;
        log_task.stopping = false;
    }
}

/* Call only from inside the window. */
void hal_flash_service(uint32_t now_ms) {
    if (board_identity_unsaved() && !hal_log_active())
        board_identity_save();
    log_flash_service(now_ms);
    board_flash_service(now_ms);
}

void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event) {
    if (!log_task.active)
        return;
    flog_sample_t s = {
        .time_ms = time_ms,
        .pressure_pa = pressure_pa,
        .altitude_cm = altitude_cm,
        .raw_pa = pp_last_read_raw_pa(),
        .temp_dc = (int16_t)lroundf(pres.last.temperature_c * 10.0f),
        .state = state,
        .thrust = under_thrust,
        .event = event,
    };
    log_plan_take(&log_plan, &s, log_keep, NULL);
}

/* A text row: script output, or a note about something the firmware did not
 * do. See flash_window.h for what a false return means. Its numeric columns
 * render empty: neither is a sample, and a zero altitude is a reading a plot
 * would draw. */
static bool log_tagged(uint32_t time_ms, uint8_t tag, const char *text, int len) {
    if (!log_task.active || len <= 0) {
        return false;
    }
    int n = flog_put_text(log_task.buf + log_task.head, LOG_BUF_SIZE - log_task.head, time_ms, tag, text, len);
    if (n == 0) {
        log_task.text_dropped += (uint32_t)len;
        return false;
    }
    log_task.head += n;
    return true;
}

bool hal_log_text(uint32_t time_ms, const char *text, int len) {
    /* Rationed: a script can flood, and script output must not crowd out the
     * flight samples it is annotating. */
    if (log_task.head >= LOG_TEXT_CEILING) {
        log_task.text_dropped += (uint32_t)(len > 0 ? len : 0);
        return false;
    }
    return log_tagged(time_ms, FLOG_TAG_LUA, text, len);
}

bool hal_log_mock(uint32_t time_ms, const char *what) {
    /* Not rationed. A mocked fire happens a handful of times in a flight and
     * is the reason the flight looks wrong afterwards; dropping it to make
     * room for samples would hide exactly the row that explains them. */
    return log_tagged(time_ms, FLOG_TAG_MOCK, what, (int)strlen(what));
}

uint32_t hal_log_text_dropped(void) {
    return log_task.text_dropped;
}

void hal_log_stop(void) {
    if (!log_task.active)
        return;
    log_plan_finish(&log_plan, log_keep, NULL);
    log_task.stopping = true;
}

bool hal_log_active(void) {
    return log_task.active;
}

void hal_firmware_commit(void) {
    pfb_firmware_commit();
}
