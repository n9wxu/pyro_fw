/*
 * HAL implementation for real Pico hardware.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "config.h"
#include "async_task.h"
#include "ms5607_driver.h"
#include "bmp280_driver.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include <lfs.h>
#include <pico_fota_bootloader/core.h>
#include "pressure_sensor.h"
#include "pyro.h"
#include <string.h>

/* ── External dependencies ────────────────────────────────────────── */

extern const struct lfs_config lfs_pico_flash_config;

/* Network (net_glue.c / http_server.c) */
void net_init(void);
void net_start(void);
void net_mdns_poll(void);
void net_mac_init(void);
void net_service(void);
void http_server_init(void);

#define BUZZER_PIN 16

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

/* ── Pressure sensor async state machine [v2 Task 4] ─────────────── */

typedef enum { PRES_IDLE, PRES_D1_CONV, PRES_D2_CONV } pres_phase_t;

/*
 * Pressure async task.  struct async_task MUST be the first member so
 * a pointer cast between pres_task_t * and async_task_t * is safe.
 */
typedef struct {
    async_task_t base; /* MUST be first */
    pres_phase_t phase;
    int sensor_type;             /* 1=MS5607  2=BMP280 */
    uint32_t sample_interval_ms; /* 1000/rate_hz */
    uint32_t d1_raw;             /* MS5607: saved D1 between phases */

    /* Ping-pong batch buffers.  back is filled by the tick function;
     * front is promoted atomically when full and read by the consumer. */
    hal_pressure_batch_t back;
    hal_pressure_batch_t front;
    volatile bool front_ready; /* true when front is valid for consumer */

    /* Latest sample for hal_pressure_read() bridge (transparent mode). */
    hal_pressure_t last;
    bool has_last;
} pres_task_t;

static pres_task_t pres;

/* Append a completed reading to the batch and update the bridge sample. */
static void pres_append(pres_task_t *p, const pressure_reading_t *r, uint32_t now_ms) {
    p->last.pressure_pa = r->pressure_pa;
    p->last.temperature_c = r->temperature_c;
    p->has_last = true;

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
}

/*
 * Pressure state machine tick.
 *
 * MS5607 (sensor_type == 1): three phases per sample
 *   IDLE      → write D1 command (~25µs) → PRES_D1_CONV, due +10ms
 *   D1_CONV   → read D1 + write D2 (~400µs) → PRES_D2_CONV, due +10ms
 *   D2_CONV   → read D2 + compensate → IDLE, due +idle_time
 *
 * BMP280 (sensor_type == 2): single phase — read output registers.
 *   No conversion wait needed (normal/continuous mode).
 */
static void pres_tick(async_task_t *base, uint32_t now_ms) {
    pres_task_t *p = (pres_task_t *)base;

    if (p->sensor_type == 1) {
        /* ── MS5607 ── */
        switch (p->phase) {
        case PRES_IDLE:
            if (ms5607_start_d1()) {
                p->phase = PRES_D1_CONV;
                p->base.next_due_ms = now_ms + MS5607_CONV_MS;
            } else {
                p->base.next_due_ms = now_ms + 50; /* back-off on I2C error */
            }
            break;

        case PRES_D1_CONV:
            if (ms5607_read_raw(&p->d1_raw) && ms5607_start_d2()) {
                p->phase = PRES_D2_CONV;
                p->base.next_due_ms = now_ms + MS5607_CONV_MS;
            } else {
                p->phase = PRES_IDLE;
                p->base.next_due_ms = now_ms + 50;
            }
            break;

        case PRES_D2_CONV: {
            uint32_t d2;
            if (ms5607_read_raw(&d2)) {
                pressure_reading_t r;
                ms5607_compensate(p->d1_raw, d2, &r);
                pres_append(p, &r, now_ms);
            }
            p->phase = PRES_IDLE;
            /* Total cycle = 2 * MS5607_CONV_MS (phases) + idle remainder.
             * At 50 Hz sample_interval = 20 ms → idle = 0 (continuous).
             * At 10 Hz sample_interval = 100 ms → idle = 80 ms sleep. */
            uint32_t idle = p->sample_interval_ms - 2u * MS5607_CONV_MS;
            p->base.next_due_ms = now_ms + idle;
            break;
        }
        }

    } else if (p->sensor_type == 2) {
        /* ── BMP280 (normal/continuous mode) ── */
        pressure_reading_t r;
        if (bmp280_read(&r))
            pres_append(p, &r, now_ms);
        p->base.next_due_ms = now_ms + p->sample_interval_ms;
    }
}

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* ── Pressure sensor ──────────────────────────────────────────────── */

static int hw_sensor_type = 0;

int hal_pressure_init(void) {
    hw_sensor_type = (int)pressure_sensor_init();
    if (hw_sensor_type > 0)
        hal_pressure_fifo_start(50); /* auto-start async sampling at 50 Hz */
    return hw_sensor_type;
}

/* Bridge: returns the most recent async sample when available.
 * Falls back to a synchronous read before the state machine has run. */
bool hal_pressure_read(hal_pressure_t *out) {
    if (pres.has_last) {
        *out = pres.last;
        return true;
    }
    pressure_reading_t r;
    if (!pressure_sensor_read(&r))
        return false;
    out->pressure_pa = r.pressure_pa;
    out->temperature_c = r.temperature_c;
    return true;
}

/* ── Pressure FIFO (v2 async batch API) ───────────────────────────── */

bool hal_pressure_fifo_start(uint8_t rate_hz) {
    if (hw_sensor_type == 0)
        return false;
    memset(&pres, 0, sizeof(pres));
    pres.base.tick = pres_tick;
    pres.base.next_due_ms = hal_time_ms(); /* run on first tick */
    pres.sensor_type = hw_sensor_type;
    pres.sample_interval_ms = (rate_hz > 0) ? (1000u / (uint32_t)rate_hz) : 100u;
    pres.phase = PRES_IDLE;
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
    return pres.base.tick != NULL;
}

/* ── Pyro ─────────────────────────────────────────────────────────── */

void hal_pyro_init(void) {
    pyro_init();
}

void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2) {
    pyro_continuity_t c1, c2;
    pyro_check_continuity(&c1, &c2);
    p1->raw_adc = c1.raw_adc;
    p1->good = c1.good;
    p1->open = c1.open;
    p1->shorted = c1.shorted;
    p2->raw_adc = c2.raw_adc;
    p2->good = c2.good;
    p2->open = c2.open;
    p2->shorted = c2.shorted;
}

void hal_pyro_fire(uint8_t channel) {
    pyro_fire(channel);
}
void hal_pyro_update(uint32_t now_ms) {
    pyro_update(now_ms);
}
bool hal_pyro_is_firing(void) {
    return pyro_is_firing();
}
bool hal_pyro_fault(uint8_t channel) {
    return pyro_fault(channel);
}

/* ── Buzzer ───────────────────────────────────────────────────────── */

void hal_buzzer_init(void) {
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);
}

void hal_buzzer_tone_on(void) {
    gpio_put(BUZZER_PIN, 1);
}
void hal_buzzer_tone_off(void) {
    gpio_put(BUZZER_PIN, 0);
}

/* Register the buzzer async task with the hardware task runner.
 * buzzer_init() calls this so the buzzer task runs alongside the
 * pressure task without any main-loop involvement. */
void hal_buzzer_task_register(async_task_t *task) {
    hw_task_register(task);
}

/* ── UART TX ring buffer (v2-10) ──────────────────────────────────── */
/*
 * hal_telemetry_send() is now non-blocking: bytes are placed into a
 * 512-byte ring buffer, and the UART0 TX ISR drains them into the
 * PL011 8-entry TX FIFO without stalling the main loop.
 *
 * Telemetry is best-effort: if the ring is full when called, the
 * remainder of the sentence is silently dropped.  The ring can hold
 * ~44 full NMEA sentences; a drop only happens if the caller is
 * substantially faster than 115200 baud (~11.5 kB/s).
 *
 * RX is unchanged: hal_serial_readline() continues to poll the RX FIFO
 * directly; no RX ISR is installed.
 */

#define UART_TX_BUF_SIZE 512 /* must be a power of 2 */
#define UART_TX_BUF_MASK (UART_TX_BUF_SIZE - 1)

static volatile uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static volatile int uart_tx_head = 0; /* producer write cursor (main loop) */
static volatile int uart_tx_tail = 0; /* consumer read cursor (ISR only) */

/*
 * Drain the ring buffer into the PL011 TX FIFO.
 * Called exclusively from the UART0 ISR — no locking needed
 * (SPSC ring: ISR owns tail, main loop owns head).
 */
static void uart_tx_drain_isr(void) {
    while (uart_is_writable(uart0)) {
        int t = uart_tx_tail;
        if (t == uart_tx_head)
            break; /* ring empty */
        uart_get_hw(uart0)->dr = uart_tx_buf[t];
        uart_tx_tail = (t + 1) & UART_TX_BUF_MASK;
    }
    /* Disable TX interrupt when ring is empty to avoid spurious re-entry */
    if (uart_tx_tail == uart_tx_head)
        hw_clear_bits(&uart_get_hw(uart0)->imsc, UART_UARTIMSC_TXIM_BITS);
}

static void uart0_irq_handler(void) {
    /* TX FIFO ≤ 1/2 full — drain ring into FIFO */
    if (uart_get_hw(uart0)->mis & UART_UARTMIS_TXMIS_BITS)
        uart_tx_drain_isr();
    /* RX: polled by hal_serial_readline() — no ISR action needed */
}

/*
 * Install the UART0 IRQ handler and leave TX interrupt disabled.
 * The interrupt is enabled on demand by hal_telemetry_send().
 * Must be called after uart_init().
 */
static void uart_tx_ring_init(void) {
    irq_set_exclusive_handler(UART0_IRQ, uart0_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    /* TX interrupt starts disabled — enabled when ring has data */
    hw_clear_bits(&uart_get_hw(uart0)->imsc, UART_UARTIMSC_TXIM_BITS);
}

/* ── Telemetry ────────────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence) {
    if (!sentence)
        return;
    /* Copy bytes into ring; drop tail of sentence if ring fills up */
    for (const char *p = sentence; *p; p++) {
        int next = (uart_tx_head + 1) & UART_TX_BUF_MASK;
        if (next == uart_tx_tail)
            break; /* ring full — drop remainder (best-effort telemetry) */
        uart_tx_buf[uart_tx_head] = (uint8_t)*p;
        uart_tx_head = next;
    }
    /* Arm TX interrupt — ISR will drain the ring into the FIFO */
    if (uart_tx_head != uart_tx_tail)
        hw_set_bits(&uart_get_hw(uart0)->imsc, UART_UARTIMSC_TXIM_BITS);
}

/* ── Filesystem ───────────────────────────────────────────────────── */

int hal_fs_mount(void) {
    lfs_t lfs;
    int err = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&lfs, &lfs_pico_flash_config);
        err = lfs_mount(&lfs, &lfs_pico_flash_config);
    }
    if (err == 0)
        lfs_unmount(&lfs);
    return err;
}

void hal_fs_unmount(void) {
    /* Each read/write mounts and unmounts internally */
}

int hal_fs_read_file(const char *path, char *buf, int max_len) {
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return -1;
    lfs_file_t f;
    int err = lfs_file_open(&lfs, &f, path, LFS_O_RDONLY);
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

int hal_fs_write_file(const char *path, const char *data, int len) {
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return -1;
    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
        lfs_unmount(&lfs);
        return -1;
    }
    lfs_file_write(&lfs, &f, data, len);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    return 0;
}

/* ── Streaming file writes ─────────────────────────────────────────── */

struct hal_file {
    lfs_t lfs;
    lfs_file_t file;
    bool open;
};

static struct hal_file hw_file;

hal_file_t *hal_fs_open(const char *path, bool append) {
    if (hw_file.open)
        return NULL;
    if (lfs_mount(&hw_file.lfs, &lfs_pico_flash_config) != LFS_ERR_OK)
        return NULL;
    int flags = LFS_O_WRONLY | LFS_O_CREAT;
    flags |= append ? LFS_O_APPEND : LFS_O_TRUNC;
    if (lfs_file_open(&hw_file.lfs, &hw_file.file, path, flags) != LFS_ERR_OK) {
        lfs_unmount(&hw_file.lfs);
        return NULL;
    }
    hw_file.open = true;
    return &hw_file;
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

/* ── Serial readline (v2, TRRS jack RX = uart0 RX) ───────────────── */

bool hal_serial_readline(char *buf, int max_len) {
    static char rx_buf[64];
    static int rx_len = 0;

    while (uart_is_readable(uart0) && rx_len < (int)(sizeof(rx_buf) - 1)) {
        char c = (char)uart_getc(uart0);
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

/* ── Sleep (v2, alarm-timer until earliest task deadline) ─────────── */

void hal_sleep_until_event(void) {
    uint32_t due = hw_tasks_next_due();
    uint32_t now = hal_time_ms();
    /* If no tasks are registered or the deadline is already past,
     * fall back to a plain WFE (wakes on any interrupt). */
    if (due == 0 || (int32_t)(due - now) <= 1) {
        __wfe();
        return;
    }
    /* sleep_until programs TIMER_ALARM0 and enters WFE.
     * Any interrupt (USB, UART RX, etc.) wakes the CPU earlier.
     * The alarm fires at the exact deadline if nothing else wakes us. */
    sleep_until(from_us_since_boot((uint64_t)due * 1000u));
}

/* ── Platform ─────────────────────────────────────────────────────── */

void hal_platform_init(void) {
    board_init();
    net_mac_init();
    tud_init(BOARD_TUD_RHPORT);
    stdio_init_all();

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    uart_tx_ring_init(); /* v2-10: non-blocking TX via ISR ring buffer */

    adc_gpio_init(26);
    adc_gpio_init(27);

    net_init();
    net_start();
    http_server_init();

    pfb_firmware_commit();

    i2c_deinit(i2c1);
}

void hal_platform_service(void) {
    tud_task();
    net_service();
    net_mdns_poll();
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

/* ── In-flight data logging [v2-9] ───────────────────────────────── */
/*
 * Architecture: hal_log_sample() places formatted CSV lines into a 512-byte
 * RAM ring.  A registered async_task_t (log_task) flushes the ring to
 * littlefs every 200ms.  The flight software never blocks on flash I/O.
 * hal_log_start()/hal_log_stop() bookend one flight log file.
 */

#define LOG_BUF_SIZE 512

typedef struct {
    async_task_t base;
    hal_file_t *file;
    char buf[LOG_BUF_SIZE];
    int head; /* write cursor */
    bool active;
    bool stopping;
} log_task_t;

static log_task_t log_task;

static void log_task_tick(async_task_t *self, uint32_t now_ms) {
    log_task_t *t = (log_task_t *)self;
    if (!t->active) {
        t->base.next_due_ms = now_ms + 200;
        return;
    }
    if (t->head > 0 && t->file) {
        hal_fs_write(t->file, t->buf, t->head);
        t->head = 0;
    }
    if (t->stopping && t->head == 0) {
        if (t->file) {
            hal_fs_close(t->file);
            t->file = NULL;
        }
        t->active = false;
        t->stopping = false;
        t->base.next_due_ms = now_ms + 200;
        return;
    }
    t->base.next_due_ms = now_ms + 200;
}

static const char *hw_mode_name(uint8_t mode) {
    switch (mode) {
    case 1:
        return "agl";
    case 2:
        return "fallen";
    case 3:
        return "speed";
    case 4:
        return "delay";
    default:
        return "none";
    }
}

void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa) {
    if (log_task.active)
        return;
    log_task.file = hal_fs_open("flight_log.csv", false);
    if (!log_task.file)
        return;
    /* write header directly — first call is not time-critical */
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "# Pyro MK1B Flight Data\n# ID: %.8s\n# Name: %.8s\n"
                     "# Pyro1: %s %u\n# Pyro2: %s %u\n"
                     "# Units: %s\n# Ground Pa: %ld\n"
                     "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
                     cfg->id, cfg->name, hw_mode_name(cfg->pyro1_mode), cfg->pyro1_value, hw_mode_name(cfg->pyro2_mode),
                     cfg->pyro2_value,
                     cfg->units == 2   ? "ft"
                     : cfg->units == 1 ? "m"
                                       : "cm",
                     (long)ground_pressure_pa);
    hal_fs_write(log_task.file, hdr, n);
    log_task.head = 0;
    log_task.active = true;
    log_task.stopping = false;
    log_task.base.tick = log_task_tick;
    log_task.base.next_due_ms = hal_time_ms() + 200;
    /* Register idempotently — only once in the task table */
    bool already = false;
    for (int i = 0; i < hw_task_count; i++)
        if (hw_tasks[i] == &log_task.base) {
            already = true;
            break;
        }
    if (!already)
        hw_task_register(&log_task.base);
}

static const char *hw_evt_name(uint8_t evt) {
    switch (evt) {
    case 1:
        return "LAUNCH";
    case 2:
        return "APOGEE";
    case 3:
        return "PYRO1";
    case 4:
        return "PYRO2";
    case 7:
        return "LANDING";
    case 9:
        return "ARMED";
    default:
        return "";
    }
}

void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event) {
    if (!log_task.active)
        return;
    char line[80];
    int n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n", (unsigned long)time_ms, (long)pressure_pa,
                     (long)altitude_cm, state, under_thrust, hw_evt_name(event));
    /* if line doesn't fit flush synchronously to avoid dropping data */
    if (log_task.head + n > LOG_BUF_SIZE && log_task.file) {
        hal_fs_write(log_task.file, log_task.buf, log_task.head);
        log_task.head = 0;
    }
    if (n > 0 && n <= LOG_BUF_SIZE) {
        memcpy(log_task.buf + log_task.head, line, n);
        log_task.head += n;
    }
}

void hal_log_stop(void) {
    if (!log_task.active)
        return;
    log_task.stopping = true;
}

bool hal_log_active(void) {
    return log_task.active;
}

void hal_firmware_commit(void) {
    pfb_firmware_commit();
}
