/*
 * Hardware Abstraction Layer for Pyro MK1B flight computer.
 *
 * This header defines the complete boundary between flight logic
 * and hardware. Flight code (flight_states.c, telemetry.c, buzzer.c)
 * includes ONLY this header — no platform-specific headers.
 *
 * Three implementations exist:
 *   src/hal_hardware.c  — real Pico hardware
 *   test/hal_test.c     — mock for unit/integration tests
 *   sim/hal_sim.c       — WASM/host simulation
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h" /* config_t for hal_config_load/save */

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void);

/* ── Pressure sensor ──────────────────────────────────────────────── */

typedef struct {
    float pressure_pa;
    float temperature_c;
} hal_pressure_t;

int hal_pressure_init(void); /* returns: 0=none, 1=ms5607, 2=bmp280 */
bool hal_pressure_read(hal_pressure_t *out);

/* ── Pressure FIFO (V2 batch API) [DD-002/003/006/007] ───────────── */
/* The FIFO decouples sensor sampling from flight software processing.
 * Producer: platform-specific (Core1, ISR, DMA).
 * Consumer: flight software calls get_buffer/release_buffer.
 *
 * Platforms implement one of:
 *   - hal_pressure_read() for polled mode (test, sim, simple HAL)
 *   - hal_pressure_fifo_*() for autonomous mode (RP2040, ESP32, STM32)
 *
 * The flight software uses whichever is available. If FIFO is available,
 * it processes batches for better efficiency and sleep behavior. */

#define HAL_PRESSURE_BATCH_SIZE 5

typedef struct {
    hal_pressure_t samples[HAL_PRESSURE_BATCH_SIZE];
    uint32_t timestamps_ms[HAL_PRESSURE_BATCH_SIZE];
    uint8_t count;
} hal_pressure_batch_t;

/* Start autonomous pressure sampling at the given rate (Hz).
 * Returns true if FIFO mode is supported, false if polled only.
 * If false, flight software falls back to hal_pressure_read(). */
bool hal_pressure_fifo_start(uint8_t rate_hz);

/* Get a filled batch of samples. Returns true if batch is ready.
 * The batch pointer is valid until hal_pressure_fifo_release(). */
bool hal_pressure_fifo_get(hal_pressure_batch_t *batch);

/* Release the batch buffer back to the producer. */
void hal_pressure_fifo_release(void);

/* Returns true if FIFO mode is active (vs polled mode). */
bool hal_pressure_fifo_active(void);

/* ── Pyro channels ────────────────────────────────────────────────── */

typedef struct {
    uint16_t raw_adc;
    bool good;
    bool open;
    bool shorted;
} hal_continuity_t;

void hal_pyro_init(void);
void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2);
void hal_pyro_fire(uint8_t channel);
void hal_pyro_update(uint32_t now_ms);
bool hal_pyro_is_firing(void);
bool hal_pyro_fault(uint8_t channel); /* FLAG pin: true = fault during fire */

/* ── Buzzer ───────────────────────────────────────────────────────── */

void hal_buzzer_init(void);
void hal_buzzer_tone_on(void);
void hal_buzzer_tone_off(void);

/* ── Telemetry output ─────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence);

/* ── Filesystem ───────────────────────────────────────────────────── */

int hal_fs_mount(void); /* returns 0 on success, <0 on error */
void hal_fs_unmount(void);
int hal_fs_read_file(const char *path, char *buf, int max_len);     /* returns bytes read, <0 on error */
int hal_fs_write_file(const char *path, const char *data, int len); /* returns 0 on success */

/* Streaming file writes */
typedef struct hal_file hal_file_t;
hal_file_t *hal_fs_open(const char *path, bool append); /* NULL on error */
int hal_fs_write(hal_file_t *f, const char *data, int len);
void hal_fs_close(hal_file_t *f);

/* ── Config [v2: replaces direct hal_fs_* in flight software] ──────── */

/* Load configuration from persistent storage into cfg.
 * Calls config_set_defaults() first, then overlays stored values.
 * Returns 0 on success, -1 if no config file (defaults were used). */
int hal_config_load(config_t *cfg);

/* Save configuration to persistent storage.
 * Returns 0 on success, -1 on error. */
int hal_config_save(const config_t *cfg);

/* ── Serial commands (ground test, DD-011) ────────────────────────── */

/* Non-blocking serial line read from the TRRS telemetry jack (same UART
 * as telemetry TX, but the RX side).
 * Returns true if a complete line was read; buf is NUL-terminated with
 * trailing CR/LF stripped.  Returns false if no complete line available. */
bool hal_serial_readline(char *buf, int max_len);

/* ── Async task runner [v2] ───────────────────────────────────────── */

/* Advance all registered async HAL state machines.
 * Call once per main loop iteration before dispatch_state().
 * Hardware HAL: runs due pressure/telemetry/log tasks.
 * Test and sim HALs implement this as a no-op. */
void hal_tasks_tick(uint32_t now_ms);

/* ── Power / sleep [v2, PWR-SLEEP-01] ────────────────────────────── */

/* Sleep the CPU until the next async task is due, a serial input event,
 * or any hardware interrupt (USB, timer).
 * Hardware HAL: sleep_until(earliest task next_due_ms) via alarm timer.
 * Test and sim HALs implement this as a no-op. */
void hal_sleep_until_event(void);

/* ── Platform (called from main, not flight code) ─────────────────── */

void hal_platform_init(void);
void hal_platform_service(void);
void hal_firmware_commit(void);

#endif
