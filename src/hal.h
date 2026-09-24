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

/* Continuity is measured with a stimulus that is SHARED between channels --
 * MK1B asserts the common enable, MK1C biases the firing bus -- so one
 * measurement yields both channels. Taking a sample and reading a channel
 * are therefore separate operations:
 *
 *   hal_pyro_sample()  performs one stimulus event and latches both results
 *   hal_pyro_get(ch)   returns the latched result for one channel
 *
 * Callers that want both channels at one instant sample once and get twice.
 * Callers that care about a single channel at a particular moment -- the
 * post-fire verify and re-fire windows, which open per channel -- sample
 * once and get only the channel whose window is open.
 *
 * A single combined call cannot serve both: it forced the per-channel sites
 * to sample twice and discard half of each result. A per-channel API cannot
 * either, because it would hide that the stimulus is shared and double the
 * current through the bridgewire on every routine check.
 *
 * channel is 1 or 2; any other value leaves *out unmodified. */
void hal_pyro_sample(void);
void hal_pyro_get(uint8_t channel, hal_continuity_t *out);
void hal_pyro_fire(uint8_t channel);
void hal_pyro_update(uint32_t now_ms);
bool hal_pyro_is_firing(void);
bool hal_pyro_fault(uint8_t channel); /* FLAG pin: true = fault during fire */

/* Claim each channel's pads for the flight software and install the real
 * operations for the channels that got them. Call once at boot, after the
 * pads have owners (pin_store_claim_pads()) and before the flight loop.
 *
 * A channel whose pads Lua already holds cannot claim them, so it is given
 * mocked operations instead -- the calls above then reach a function that
 * touches no hardware, because the real one was never installed for it.
 * pads_of answers which pads a channel switches, as a pad_claim.h mask. Passed
 * in rather than looked up, so this layer stays free of the pin assignment and
 * of Lua -- the caller already knows both.
 *
 * Returns how many channels the flight software kept. Every mocked operation
 * is logged and counted; see pyro_release.h and pad_claim.h. */
int hal_pyro_claim_channels(uint32_t (*pads_of)(uint8_t channel));

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

/* Whether the last hal_fs_mount() succeeded.
 *
 * hal_platform_init() mounts before the flight software exists, and its
 * return was discarded, so a board whose filesystem would not mount reported
 * itself healthy. BEEP_FS_FAIL has been defined since the beginning and was
 * emitted from nowhere. */
bool hal_fs_healthy(void);
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

/* Non-blocking line read from the TRRS telemetry jack, on the RX side of the
 * UART that carries telemetry TX.
 *
 * Returns true when a complete line was read, leaving buf NUL-terminated with
 * any trailing CR or LF stripped. Returns false when no complete line is
 * available yet. */
bool hal_serial_readline(char *buf, int max_len);

/* ── In-flight data logging ──────────────────────────────────────
 *
 * Fire and forget: every call below buffers into RAM and returns, so none of
 * them blocks the flight software or reaches flash. The platform decides
 * when the buffer is written.
 *
 * The flight code calls hal_log_start() at LAUNCH, hal_log_sample() for each
 * ASCENT, DESCENT and LANDED sample, and hal_log_stop() once LANDED has
 * finalised. Nothing calls these during PAD_IDLE.
 *
 * See REQUIREMENTS.md v2-9. */
void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa);
void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event);
void hal_log_stop(void);
bool hal_log_active(void);

/* ── Async task runner [v2] ───────────────────────────────────────── */

/* Advance every registered async HAL state machine. Call once per main-loop
 * iteration, before dispatch_state().
 *
 * The hardware HAL runs the pressure and telemetry tasks that are due. The
 * test and sim HALs implement this as a no-op. */
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
