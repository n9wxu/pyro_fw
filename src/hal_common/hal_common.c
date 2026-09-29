/*
 * Board-independent HAL implementation for RP2040 targets.
 *
 * Implements hal.h in terms of src/board_if.h. Contains no pin numbers and
 * no board names; everything board-specific lives in boards/<name>/.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "flight_events.h"
#include "flash_op.h"
#include "hal_storage.h"
#include "lfs_mount.h"
#include "vfs.h"
#if PYRO_HAS_SD
#include "sd_card.h"
#endif
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "semphr.h"
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
#include "hardware/sync.h"
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
#if PYRO_HAS_BENCH_FLIGHT
#include "bench_flight.h"
#endif
#include <math.h>
#include <string.h>

/* ── External dependencies ────────────────────────────────────────── */

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
static void pres_append(pres_task_t *p, const pressure_reading_t *r_in) {
    pressure_reading_t rb = *r_in;
    const pressure_reading_t *r = &rb;
#if PYRO_HAS_BENCH_FLIGHT
    /* [SIM-02] After the trace took the sensor's reading. */
    bench_flight_pressure(rb.time_us, flight_get_state() == LANDED, &rb.pressure_pa);
#endif
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

#if PYRO_HAS_BENCH_FLIGHT
/* [SIM-03] Good until fired, open after, as a lit charge. */
static void bench_get(uint8_t channel, hal_continuity_t *out) {
    bool fired = bench_flight_fired(channel);
    *out = (hal_continuity_t){.raw_adc = fired ? 0 : 1000, .good = !fired, .open = fired, .shorted = false};
}

static void bench_fire(uint8_t channel) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bench_flight_fire(channel, now);
    const flight_context_t *fc = flight_get_context();
    char note[40];
    snprintf(note, sizeof(note), "pyro%u fire: bench flight", (unsigned)channel);
    hal_log_mock(fc ? flight_elapsed_ms(fc, now) : now, note);
    char line[56];
    snprintf(line, sizeof(line), "!MOCK %s\r\n", note);
    hal_telemetry_send(line);
}
#endif

void hal_pyro_get(uint8_t channel, hal_continuity_t *out) {
#if PYRO_HAS_BENCH_FLIGHT
    if (bench_flight_mocked()) {
        bench_get(channel, out);
        return;
    }
#endif
    pyro_ch(channel)->get(channel, out);
}

void hal_pyro_fire(uint8_t channel) {
#if PYRO_HAS_BENCH_FLIGHT
    if (bench_flight_mocked()) {
        bench_fire(channel);
        return;
    }
#endif
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
#if PYRO_HAS_BENCH_FLIGHT
    if (bench_flight_mocked())
        return bench_flight_firing(to_ms_since_boot(get_absolute_time()));
#endif
    return pyro_is_firing();
}
bool hal_pyro_fault(uint8_t channel) {
#if PYRO_HAS_BENCH_FLIGHT
    if (bench_flight_mocked())
        return false;
#endif
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

static spin_lock_t *uart_tx_lock;

static void telem_uart_irq_handler(void) {
    if (uart_get_hw(tuart())->mis & UART_UARTMIS_TXMIS_BITS) {
        uint32_t irq = spin_lock_blocking(uart_tx_lock);
        uart_tx_drain_isr();
        spin_unlock(uart_tx_lock, irq);
    }
}

/* The flight task writes telemetry and the net task its diagnostics, on
 * different cores, into one ring, which the TX interrupt drains. A hardware
 * spin lock with interrupts off makes each insert and each drain whole; it is
 * held for one line's copy, so the flight task's wait on it is bounded by
 * that. */
static void uart_tx_ring_init(void) {
    uart_tx_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
    irq_set_exclusive_handler(board_uart_irq(), telem_uart_irq_handler);
    irq_set_enabled(board_uart_irq(), true);
    hw_clear_bits(&uart_get_hw(tuart())->imsc, UART_UARTIMSC_TXIM_BITS);
}

/* ── Telemetry ────────────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence) {
    if (!sentence || !uart_tx_lock)
        return;
    uint32_t irq = spin_lock_blocking(uart_tx_lock);

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
    spin_unlock(uart_tx_lock, irq);
}

/* ── Filesystem ───────────────────────────────────────────────────── */

lfs_t g_lfs;
static bool fs_ok;

bool lfs_mounted(void) {
    return fs_ok;
}

/* [WEB-API-08, DD-058] File uses other than the flight log's, open now. None
 * may start while the log holds the filesystem. */
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

/* Once, at boot, before the scheduler: formats a blank board. */
int hal_fs_mount(void) {
    if (fs_ok)
        return 0;
    int err = lfs_mount(&g_lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&g_lfs, &lfs_pico_flash_config);
        err = lfs_mount(&g_lfs, &lfs_pico_flash_config);
    }
    fs_ok = (err == 0);
    return err;
}

void hal_fs_unmount(void) {
    /* Mounted for the life of the firmware. */
}

/* One cache buffer for the whole-file helpers below; littlefs's own lock
 * serialises each call, and this one serialises the buffer across a whole
 * open-read-close. Never the flight task's. */
static uint8_t whole_buf[LFS_FILE_BUF_SIZE];
static SemaphoreHandle_t whole_mutex;
static StaticSemaphore_t whole_mutex_buf;

static bool whole_take(void) {
    if (!rtos_running())
        return true;
    if (rtos_in_flight_task())
        return false;
    if (!whole_mutex)
        whole_mutex = xSemaphoreCreateMutexStatic(&whole_mutex_buf);
    return xSemaphoreTake(whole_mutex, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void whole_give(void) {
    if (rtos_running() && whole_mutex)
        xSemaphoreGive(whole_mutex);
}

static int read_from(lfs_t *lfs, const char *path, char *buf, int max_len) {
    lfs_file_t f;
    struct lfs_file_config fc = {.buffer = whole_buf};
    int err = lfs_file_opencfg(lfs, &f, path, LFS_O_RDONLY, &fc);
    if (err == LFS_ERR_NOENT)
        return -2;
    if (err != LFS_ERR_OK)
        return -1;
    lfs_ssize_t n = lfs_file_read(lfs, &f, buf, max_len);
    lfs_file_close(lfs, &f);
    return (int)n;
}

static int read_vfs(const char *path, char *buf, int max_len) {
    vfs_file_t f;
    int err = vfs_open(&f, path, VFS_RD, whole_buf);
    if (err < 0)
        return err;
    int n = vfs_read(&f, buf, (uint32_t)max_len);
    vfs_close(&f);
    return n;
}

static int read_file(const char *path, char *buf, int max_len) {
    if (!whole_take())
        return -1;
    int n;
    if (fs_ok) {
        n = read_vfs(path, buf, max_len);
    } else {
        /* Before hal_fs_mount(): board_identity_init() reads /serial.txt
         * this way. Read-only, and never formats, so a blank board draws a
         * MAC and enumerates instead of waiting out an 8 MB format. */
        lfs_t lfs;
        n = -1;
        if (lfs_mount(&lfs, &lfs_pico_flash_config) == LFS_ERR_OK) {
            n = read_from(&lfs, path, buf, max_len);
            lfs_unmount(&lfs);
        }
    }
    whole_give();
    return n;
}

int hal_fs_read_file(const char *path, char *buf, int max_len) {
    int err = hal_fs_enter();
    if (err != 0)
        return err;
    int n = read_file(path, buf, max_len);
    hal_fs_leave();
    return n;
}

/* ── Files the flight task reads ──────────────────────────────────
 *
 * The pad marker, read by the flight task's recovery decision and written by
 * the storage task: a seqlock over one RAM copy, so the reader never waits and
 * a writer that races it costs the reader a retry. A torn read the retries do
 * not settle fails the marker's own check and reads as no marker -- a cold
 * boot, the safe answer. */
#define CACHED_MAX 64

typedef struct {
    const char *path;
    volatile uint32_t seq;
    int len; /* -2: no such file */
    uint8_t data[CACHED_MAX];
} cached_file_t;

static cached_file_t cached[] = {{PAD_MARKER_PATH, 0, -2, {0}}};

static cached_file_t *cached_find(const char *path) {
    for (unsigned i = 0; i < sizeof(cached) / sizeof(cached[0]); i++)
        if (strcmp(cached[i].path, path) == 0)
            return &cached[i];
    return NULL;
}

static void cached_store(cached_file_t *c, const char *data, int len) {
    c->seq++;
    __dmb();
    if (len > CACHED_MAX)
        len = CACHED_MAX;
    if (len > 0)
        memcpy(c->data, data, (size_t)len);
    c->len = len;
    __dmb();
    c->seq++;
}

int hal_fs_read_cached(const char *path, char *buf, int max_len) {
    cached_file_t *c = cached_find(path);
    if (!c)
        return -1;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s = c->seq;
        if (s & 1u)
            continue;
        __dmb();
        int n = c->len;
        if (n > max_len)
            n = max_len;
        if (n > 0)
            memcpy(buf, c->data, (size_t)n);
        __dmb();
        if (c->seq == s)
            return n;
    }
    return -1;
}

/* At boot, after the mount: every cached file, read once. */
static void cached_fill(void) {
    for (unsigned i = 0; i < sizeof(cached) / sizeof(cached[0]); i++) {
        char buf[CACHED_MAX];
        int n = read_file(cached[i].path, buf, (int)sizeof(buf));
        cached_store(&cached[i], buf, n >= 0 ? n : -2);
    }
}

static int write_file(const char *path, const char *data, int len) {
    if (!fs_ok || !whole_take())
        return -1;
    vfs_file_t f;
    int rc = -1;
    if (vfs_open(&f, path, VFS_WR, whole_buf) == 0) {
        bool wrote = vfs_write(&f, data, (uint32_t)len) == len;
        /* Close commits: on littlefs a failed close leaves the previous file
         * whole. */
        rc = (vfs_close(&f) == 0 && wrote) ? 0 : -1;
    }
    /* A configuration file written to the card is copied into littlefs. */
    if (rc == 0)
        vfs_mirror_one(path);
    whole_give();
    cached_file_t *c = cached_find(path);
    if (c && rc == 0)
        cached_store(c, data, len);
    return rc;
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

/* One at a time, on the whole-file helpers' buffer, which it holds from open
 * to close. */
struct hal_file {
    vfs_file_t file;
    bool open;
};

static struct hal_file hw_file;

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (!fs_ok || hw_file.open || hal_fs_enter() != 0)
        return NULL;
    if (!whole_take()) {
        hal_fs_leave();
        return NULL;
    }
    if (vfs_open(&hw_file.file, path, VFS_WR | (append ? VFS_APPEND : 0), whole_buf) != 0) {
        whole_give();
        hal_fs_leave();
        return NULL;
    }
    hw_file.open = true;
    return &hw_file;
}

int hal_fs_write(hal_file_t *f, const char *data, int len) {
    if (!f || !f->open)
        return -1;
    return vfs_write(&f->file, data, (uint32_t)len);
}

void hal_fs_close(hal_file_t *f) {
    if (!f || !f->open)
        return;
    vfs_close(&f->file);
    f->open = false;
    whole_give();
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

    /* Before net_mac_init() and the net task's tud_init(), both of which
     * consume it: the MAC goes into the ECM descriptor and the subnet into the
     * DHCP server, and the host reads each exactly once, at enumeration.
     *
     * Reads /serial.txt via hal_fs_read_file(), which mounts read-only and
     * does NOT format on failure -- so a blank board draws a MAC and
     * enumerates instead of waiting out an 8 MB format. */
    board_identity_init();
    net_mac_init();
    /* tud_init() is the net task's, on core1: TinyUSB's interrupt and its
     * task must share a core, since OPT_OS_NONE guards its queue by masking
     * the interrupt on the calling core only. */
    /* stdio_init_all() removed — we own the telemetry UART exclusively for ISR-driven
     * telemetry TX and ground-test RX.  No SDK stdio drivers needed. */

    uart_init(tuart(), 115200); /* pins were assigned by board_hw_init() */
    uart_tx_ring_init();        /* v2-10: non-blocking TX via ISR ring buffer */

    net_init();
    net_start();
    http_server_init();

    /* Mounted once, for good: every task shares this mount. */
    hal_fs_mount();
    cached_fill();

#if PYRO_HAS_SD
    /* Before flight_init() loads the configuration, which is the card's when
     * there is one [DD-076]. No card, or one that will not mount: every file
     * stays in littlefs. */
    if (sd_start() == 0)
        vfs_mirror();
#endif

    /* The sensor's I2C is its bring-up's to set up: pressure_sensor_begin()
     * resets the peripheral and recovers the bus itself. */
}

/* The net task's pass, by part: TinyUSB, lwIP, HTTP transport, mDNS. */
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
 * The flight task only fills a ring; the storage task empties it into the
 * file. Nothing on the flight path waits or reaches flash: not the first
 * sample, not a full ring, not the file's own creation.
 *
 * One producer, one consumer, no lock. The flight task writes head and the
 * state's IDLE -> RUNNING -> STOPPING steps; the storage task writes tail and
 * STOPPING -> IDLE. Both counters only grow, so used = head - tail. */

/* Binary records (flight_log.h), rendered as CSV only when downloaded
 * [DD-062]. 4 kB holds 1.9 s of samples at 100 Hz, enough to span the launch
 * shock window below; at a row a second it would hold three minutes, so the
 * holdoff also ends on a timer. */
#define LOG_BUF_SIZE 4096u
#define LOG_MASK (LOG_BUF_SIZE - 1u)
#define LOG_FLUSH_MS 200u
#define LOG_HOLDOFF_MAX_MS 2000u /* [FLT-LOG-05] */

/* Written at once from here up, without waiting for the flush timer. */
#define LOG_WATERMARK (LOG_BUF_SIZE - 96u)

/* Script text shares the ring with the flight samples and ranks below them,
 * so it is admitted only in the lower half: a chatty script loses its own
 * lines rather than a single altitude reading. */
#define LOG_TEXT_CEILING (LOG_BUF_SIZE / 2u)

/* How much flight a power loss can take with it. littlefs publishes what a
 * file holds only on sync or close, and the log closes at LANDED, so a flight
 * that never lands -- a crash, a battery that lets go on impact -- would
 * leave an empty file. */
#define LOG_SYNC_MS 1000u

/* The largest record the producer builds: a text row. */
#define LOG_REC_MAX 96

_Static_assert((LOG_BUF_SIZE & LOG_MASK) == 0, "the ring's size is a power of two");

enum { LOG_IDLE, LOG_RUNNING, LOG_STOPPING };

typedef struct {
    uint8_t buf[LOG_BUF_SIZE];
    volatile uint32_t head; /* the flight task */
    volatile uint32_t tail; /* the storage task */
    volatile uint8_t state; /* see above */
    uint32_t dropped;       /* sample bytes the ring could not hold */
    uint32_t text_dropped;  /* script bytes refused: not a lost sample */
    /* The storage task's own. */
    vfs_file_t file;
    bool file_open;
    uint32_t next_due_ms;
    uint32_t next_sync_ms;
    bool unsynced;
    /* No flash write until the ring has filled once. Launch shock is the
     * likeliest cause of a brownout -- a battery connector bouncing -- and a
     * flash write in progress is the worst moment to lose power, so the first
     * seconds of the flight live in RAM. Cleared on the first watermark hit
     * or at LOG_HOLDOFF_MAX_MS, after which flushing is periodic. */
    bool launch_holdoff;
    uint32_t started_ms;
} log_ring_t;

static log_ring_t log_ring;
static uint8_t log_file_buf[LFS_FILE_BUF_SIZE];
static log_plan_t log_plan;

static uint32_t log_used(void) {
    return log_ring.head - log_ring.tail;
}

/* The flight task's side. False, and nothing written, when it does not fit. */
static bool log_put(const uint8_t *rec, uint32_t n) {
    if (n > LOG_BUF_SIZE - log_used())
        return false;
    uint32_t h = log_ring.head;
    for (uint32_t i = 0; i < n; i++)
        log_ring.buf[(h + i) & LOG_MASK] = rec[i];
    __dmb();
    log_ring.head = h + n;
    return true;
}

static void log_keep(void *ctx, const flog_sample_t *s) {
    (void)ctx;
    uint8_t rec[FLOG_SAMPLE_BYTES + 8];
    int n = flog_put_sample(rec, (int)sizeof(rec), s);
    if (n <= 0 || !log_put(rec, (uint32_t)n))
        log_ring.dropped += FLOG_SAMPLE_BYTES;
}

uint32_t hal_log_dropped(void) {
    return log_ring.dropped;
}

void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa) {
    if (log_ring.state != LOG_IDLE)
        return;

    /* Called at liftoff, so it opens no file and writes no flash: a launch
     * that waited for an erase would be a launch detected late. The storage
     * task is idle on the ring until the state below says RUNNING. */
    log_ring.dropped = 0;
    log_ring.head = log_ring.tail = 0;
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
    int n = flog_put_header(log_ring.buf, (int)LOG_BUF_SIZE, &h);
    log_ring.head = n > 0 ? (uint32_t)n : 0u;
    log_plan_init(&log_plan, cfg->log_rate);
    __dmb();
    log_ring.state = LOG_RUNNING;
}

/* The storage task's side from here to hal_storage_service(). A failure costs
 * only a delay: the ring keeps its bytes until a write takes them. Only a
 * ring that filled faster than the task drained it loses anything. */
static void log_sync_if_due(uint32_t now_ms) {
    if (!log_ring.file_open || !log_ring.unsynced || (int32_t)(now_ms - log_ring.next_sync_ms) < 0)
        return;
    if (vfs_sync(&log_ring.file) == 0)
        log_ring.unsynced = false;
    log_ring.next_sync_ms = now_ms + LOG_SYNC_MS;
}

/* Everything the ring holds now, in at most two spans. */
static void log_write_out(void) {
    uint32_t h = log_ring.head;
    __dmb();
    while (log_ring.tail != h) {
        uint32_t t = log_ring.tail;
        uint32_t off = t & LOG_MASK;
        uint32_t span = h - t;
        if (span > LOG_BUF_SIZE - off)
            span = LOG_BUF_SIZE - off;
        int n = vfs_write(&log_ring.file, log_ring.buf + off, span);
        if (n <= 0)
            return; /* the bytes stay: the next pass retries */
        __dmb();
        log_ring.tail = t + (uint32_t)n;
        log_ring.unsynced = true;
    }
}

static void log_service(uint32_t now_ms) {
    uint8_t st = log_ring.state;
    if (st == LOG_IDLE)
        return;

    if (!log_ring.file_open) {
        if (!fs_ok || vfs_open(&log_ring.file, FLOG_PATH, VFS_WR, log_file_buf) != 0)
            return; /* the next pass retries; the ring keeps the rows */
        log_ring.file_open = true;
        log_ring.next_due_ms = now_ms + LOG_FLUSH_MS;
        log_ring.next_sync_ms = now_ms;
        log_ring.unsynced = false;
        log_ring.launch_holdoff = true;
        log_ring.started_ms = now_ms;
    }

    bool full = log_used() >= LOG_WATERMARK;
    if (full || now_ms - log_ring.started_ms >= LOG_HOLDOFF_MAX_MS)
        log_ring.launch_holdoff = false;
    /* The timer does not fire during the holdoff; a full ring and a stop
     * always do, so nothing is ever dropped to keep the flash quiet. */
    bool due = !log_ring.launch_holdoff && (int32_t)(now_ms - log_ring.next_due_ms) >= 0;
    if (full || st == LOG_STOPPING)
        due = true;
    if (!due) {
        /* In a pass with no write in it, so a sync and a block write never
         * share one pass's stop. */
        log_sync_if_due(now_ms);
        return;
    }

    log_write_out();
    log_ring.next_due_ms = now_ms + LOG_FLUSH_MS;

    if (st == LOG_STOPPING && log_ring.tail == log_ring.head) {
        vfs_close(&log_ring.file);
        log_ring.file_open = false;
        __dmb();
        log_ring.state = LOG_IDLE;
    }
}

/* Weak and empty: most boards queue no flash work of their own. */
__attribute__((weak)) void board_flash_service(uint32_t now_ms) {
    (void)now_ms;
}

void hal_storage_service(uint32_t now_ms) {
    if (board_identity_unsaved() && !hal_log_active())
        board_identity_save();
    log_service(now_ms);
    board_flash_service(now_ms);
}

void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event) {
    if (log_ring.state != LOG_RUNNING)
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
 * do. Its numeric columns render empty: neither is a sample, and a zero
 * altitude is a reading a plot would draw. */
static bool log_tagged(uint32_t time_ms, uint8_t tag, const char *text, int len) {
    if (log_ring.state != LOG_RUNNING || len <= 0)
        return false;
    uint8_t rec[LOG_REC_MAX];
    int n = flog_put_text(rec, (int)sizeof(rec), time_ms, tag, text, len);
    if (n <= 0 || !log_put(rec, (uint32_t)n)) {
        log_ring.text_dropped += (uint32_t)len;
        return false;
    }
    return true;
}

bool hal_log_text(uint32_t time_ms, const char *text, int len) {
    /* Rationed: a script can flood, and script output must not crowd out the
     * flight samples it is annotating. */
    if (log_used() >= LOG_TEXT_CEILING) {
        log_ring.text_dropped += (uint32_t)(len > 0 ? len : 0);
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
    return log_ring.text_dropped;
}

void hal_log_stop(void) {
    if (log_ring.state != LOG_RUNNING)
        return;
    log_plan_finish(&log_plan, log_keep, NULL);
    __dmb();
    log_ring.state = LOG_STOPPING;
}

bool hal_log_active(void) {
    return log_ring.state != LOG_IDLE;
}

/* pico_fota_bootloader rolls back to the previous image unless the new one
 * commits before its next reboot, so an image that crash-loops reverts by
 * itself. Committed by the storage task once every task has run (see
 * storage_task.c), not at boot: a commit before the scheduler would keep an
 * image that dies the moment its tasks start. Under the lockout: the library
 * masks interrupts and writes flash itself. */
static void commit_op(void *arg) {
    (void)arg;
    pfb_firmware_commit();
}

void hal_firmware_commit(void) {
    /* Only an image on its first boot after an update has anything to
     * commit; every other boot writes no flash here. */
    if (pfb_is_after_firmware_update())
        flash_op(commit_op, NULL);
}
