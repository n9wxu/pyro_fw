/*
 * The boundary between the flight software and the hardware. Implemented by
 * src/hal_common/hal_common.c (the RP2040 boards), test/hal_test.c and
 * boards/sim/hal_sim.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "async_task.h"

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void);

/* Why the processor started this time. A brownout reads as a power event and
 * cannot be told from someone connecting the battery -- see brownout.h. */
#include "brownout.h"
reset_cause_t hal_reset_cause(void);

/* ── Pressure sensor ──────────────────────────────────────────────── */

/* Starts bringing the pressure sensor up [DD-053]. On hardware the pressure
 * task takes it a step a loop, then samples what it found, calling pp_feed()
 * for each reading -- see pressure_processing.h. */
void hal_pressure_init(void);

/* A pressure_sensor_type_t (pressure_sensor.h): NONE, MS5607 or BMP280; -1
 * while it is still being brought up. */
int hal_pressure_sensor(void);

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
/* True from the moment hal_pyro_fire() accepts a command until the channel is
 * de-energised. The flight software reads it straight after the call as the
 * board's acknowledgement: false there means nothing was energised. */
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

/* Called once from buzzer_init(); hal_tasks_tick() then drives the task,
 * which must outlive the program. */
void hal_buzzer_task_register(struct async_task *task);

/* ── Telemetry output ─────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence);

/* ── Filesystem ───────────────────────────────────────────────────── */

int hal_fs_mount(void); /* returns 0 on success, <0 on error */

/* Whether the boot's hal_fs_mount() succeeded. hal_platform_init() mounts
 * before the flight software exists, so this is how it learns. */
bool hal_fs_healthy(void);
void hal_fs_unmount(void);
/* [WEB-API-08] From launch until its tail is flushed the flight log holds
 * the filesystem, and every other file call returns HAL_FS_LOCKED
 * (hal_fs_open(), NULL). HAL_FS_ERROR is any other failure, and HAL_FS_NOENT
 * a read of a file that does not exist -- the only failure after which a
 * caller may write a fresh file in its place. */
#define HAL_FS_ERROR (-1)
#define HAL_FS_NOENT (-2)
#define HAL_FS_LOCKED (-3)
int hal_fs_read_file(const char *path, char *buf, int max_len);     /* bytes read, or one of the above */
int hal_fs_write_file(const char *path, const char *data, int len); /* 0 on success */

/* A small file the flight software reads while it flies -- the pad marker
 * (brownout.h). Never blocks and never waits on the filesystem: the platform
 * serves it from RAM, filled at boot and refreshed by hal_fs_write_file().
 * Same returns as hal_fs_read_file(), without HAL_FS_LOCKED. */
int hal_fs_read_cached(const char *path, char *buf, int max_len);

/* Streaming file writes */
typedef struct hal_file hal_file_t;
hal_file_t *hal_fs_open(const char *path, bool append); /* NULL on error */
int hal_fs_write(hal_file_t *f, const char *data, int len);
void hal_fs_close(hal_file_t *f);

/* ── Config ───────────────────────────────────────────────────────── */

/* [CFG-05, FLT-BOOT-02, FLT-BOOT-03] config.ini over the defaults. 0: loaded.
 * -1: no file, so the defaults were written for the next boot. -2: the file
 * could not be read whole; cfg holds the defaults, the file is left as it is,
 * and the failure is said on the telemetry UART. See config_from_file(). */
int hal_config_load(config_t *cfg);

/* 0 on success, -1 on error. */
int hal_config_save(const config_t *cfg);

/* ── Serial commands (ground test, DD-011) ────────────────────────── */

/* Non-blocking line read from the RX side of the telemetry UART.
 *
 * True when a complete line was read, leaving buf NUL-terminated without its
 * CR or LF. A line longer than the platform's line buffer is discarded whole,
 * up to its end; one longer than max_len - 1 is cut to fit. */
bool hal_serial_readline(char *buf, int max_len);

/* ── In-flight data logging [FLT-LOG-05] ─────────────────────────
 *
 * Every call buffers into RAM and returns; the platform decides when the
 * buffer is written. hal_log_start() at launch, hal_log_sample() for each
 * airborne and LANDED sample, hal_log_stop() once LANDED has finalised. */
void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa);
void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm, uint8_t state, uint8_t under_thrust,
                    uint8_t event);
void hal_log_stop(void);
bool hal_log_active(void);

/* ── Ground test pin [GND-TEST-05, GND-TEST-12] ────────────────────
 *
 * Whether the ground test switch is closed now. False where no ground test
 * pin is assigned. A switch to ground on one pad, or a switch across two
 * pads, as pins.ini says; the platform hides which. */
bool hal_ground_test_asserted(void);

/* ── Async task runner ────────────────────────────────────────────── */

/* Advance every registered async HAL state machine that is due: the pressure
 * sensor, the buzzer and the ground test switch. Once a loop, before
 * dispatch_state(). */
void hal_tasks_tick(uint32_t now_ms);

/* ── Power / sleep ────────────────────────────────────────────────── */

/* A no-op on every platform, and nothing calls it: the flight task sleeps
 * between its periods (rtos_tasks.h). */
void hal_sleep_until_event(void);

/* ── Platform (called from main, not flight code) ─────────────────── */

void hal_platform_init(void);
void hal_platform_service(void);

/* False when this image is running on a board it was not built for
 * (board_selftest.h), at every boot and not only after an update. The flight
 * software must then refuse to arm or fire. */
bool hal_board_image_ok(void);

/* [GND-TEST-12] The ground test switch's pads, from pins.ini (GT_WIRING_* in
 * ground_test_switch.h), once they are claimed. Nothing is read before this, and
 * nothing at all for GT_WIRING_NONE. */
void hal_ground_test_configure(uint8_t wiring, uint8_t pin, uint8_t drive_pin);

#endif
