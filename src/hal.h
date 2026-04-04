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
#include "config.h"     /* config_t for hal_config_load/save */
#include "async_task.h" /* async_task_t for hal_buzzer_task_register */

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void);

/* ── Pressure sensor ──────────────────────────────────────────────── */

/* Initialize the pressure sensor hardware.
 * Returns: 0=none, 1=ms5607, 2=bmp280.
 * On hardware, this starts the autonomous sampling async task which
 * calls pp_feed() for each raw sample — see pressure_processing.h. */
int hal_pressure_init(void);

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

/* Register the buzzer async task with the platform task runner.
 * Called once from buzzer_init().  The task pointer must remain valid
 * for the lifetime of the program (it is a static in buzzer.c).
 * Hardware: delegates to hw_task_register() in hal_hardware.c.
 * Test/sim: stores the pointer; hal_tasks_tick() will drive it. */
void hal_buzzer_task_register(struct async_task *task);

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

/* ── In-flight data logging [v2-9] ───────────────────────────────── */
/* Fire-and-forget logging — not called during PAD_IDLE.
 * hal_log_start() called at LAUNCH: opens flight.csv, writes header.
 * hal_log_sample() called for every ASCENT/DESCENT/LANDED sample.
 * hal_log_stop() called at end of flight (LANDED + finalize).
 * The HAL buffers samples in RAM; flushes asynchronously during
 * hal_tasks_tick() so the call never blocks the flight software. */
void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa);
void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event);
void hal_log_stop(void);
bool hal_log_active(void);

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
