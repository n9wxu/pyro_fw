/*
 * Mock state declarations for unit tests.
 * The actual mock implementations are in hal_test.c.
 * Test files include this to access mock control variables.
 */
#ifndef TEST_MOCKS_H
#define TEST_MOCKS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Mock state (defined in hal_test.c) ───────────────────────────── */

typedef struct {
    float pressure_pa;
    float temperature_c;
    int sensor_type;
    uint32_t pending_until_ms; /* hal_pressure_sensor() is -1 before this */
} mock_pressure_t;

typedef struct {
    uint16_t p1_adc;
    uint16_t p2_adc;
    bool p1_good;
    bool p2_good;
    bool p1_open;
    bool p2_open;
    bool firing;
    bool fault; /* injectable fault state */
    int fire_count; /* pulses commanded, energised or not */
    uint8_t last_fire_channel;
    /* The board takes the command and energises nothing: its protection has
     * acted, or its firing path would not come up. */
    bool energises_nothing;
    uint32_t pulse_ms; /* how long a pulse energises a channel */
    /* A fired channel gives no verdict until this long after its pulse: the
     * board's next check [PYR-VERIFY-01]. */
    uint32_t verdict_after_ms;
    bool has_pulsed[2];
    uint32_t pulsed_at_ms[2];
    uint32_t pulse_start_ms;
    int sample_count; /* hal_pyro_sample() calls; one shared stimulus each */
} mock_pyro_t;

/* Every pulse commanded, in order. */
#define MOCK_PULSES_MAX 256
typedef struct {
    uint32_t start_ms;
    uint8_t channel;
    bool energised;
} mock_pulse_t;
extern mock_pulse_t mock_pulses[MOCK_PULSES_MAX];
extern int mock_pulse_count;

/* [PYR-BOARD-01] What the mocked board permits. */
#include "../src/hal.h"
extern hal_pyro_limits_t mock_pyro_limits;
extern reset_cause_t mock_reset_cause;

#define MOCK_UART_BUF_SIZE 32768

extern mock_pressure_t mock_pressure;
extern mock_pyro_t mock_pyro;

/* Which pads a pyro channel switches, in the host tests: MK1A's numbering,
 * the channel's own element plus the common. The flight software claims these
 * at reset; a test that wants a release gives them to Lua first. */
uint32_t mock_pyro_pads(uint8_t channel);

/* The last mocked operation reported, and how many there have been. */
extern char mock_pyro_last_note[64];
extern int mock_pyro_notes;
extern char mock_uart_buf[MOCK_UART_BUF_SIZE];
extern int mock_uart_len;
extern uint32_t mock_time_ms;
extern uint32_t mock_fs_locked_count; /* file calls refused while the flight log held the filesystem */
int mock_fs_peek(const char *path, char *buf, int max_len); /* a test's look at a file, past the lock */
/* The flight log as its download renders it: CSV, NUL-terminated. */
int test_flight_log_csv(char *buf, int max_len);

/* ── XIP stall simulation ─────────────────────────────────────────── */
/* On RP2040, flash writes disable XIP (Execute In Place), stalling the
 * CPU for the duration of the erase+write. This simulates that effect
 * by advancing mock_time_ms on every flash write operation.
 *
 * Typical RP2040 flash timings:
 *   Sector erase:  ~50ms
 *   Page write:    ~2-5ms
 *   Total per littlefs commit: ~60-200ms
 *
 * Set mock_xip_stall_ms > 0 to enable. Default 0 (no stall).
 */
extern uint32_t mock_xip_stall_ms;       /* time added per flash write op */
extern uint32_t mock_xip_total_stall_ms; /* cumulative stall time */
extern int mock_xip_stall_count;         /* number of stall events */

/* ── The estimators' log rows [SNS-EST-07] ────────────────────────── */
#define MOCK_ESTIMATOR_ROWS 512
extern char mock_estimator_rows[MOCK_ESTIMATOR_ROWS][65];
extern uint32_t mock_estimator_row_ms[MOCK_ESTIMATOR_ROWS];
extern int mock_estimator_row_count;

/* ── Sensor model ─────────────────────────────────────────────────── */
/* Off by default: a test that sets none of these sees the smooth signal it
 * always has. Every reading passes the HAL's own range check, and is
 * truncated to whole pascals as hal_common.c does. */
extern uint32_t mock_sample_interval_ms; /* 20: the MS5607 at 50 Hz */
extern bool mock_sensor_stuck;           /* every reading repeats the last, exactly */
extern float mock_noise_rms_pa;          /* Gaussian noise on every reading */
extern uint32_t mock_noise_seed;         /* the same seed gives the same noise */
extern int32_t mock_glitch_pa;           /* added to each of the next ... */
extern int mock_glitch_samples;          /* ... this many readings */

/* The MS5607's schedule on the hardware (DD-051, DD-065): each loop takes the
 * conversion its one-shot finished and commands the next, the temperature
 * once in ten. A reading is the pressure at the middle of its conversion,
 * 4.5 ms in, taken 5.5 ms later -- or later still if a stall holds the loop.
 * mock_one_shot runs the schedule alone; mock_stall_model adds core0's stalls,
 * 40-73 ms, about 1.3 times a second (DD-035). While one lasts nothing on core0
 * runs: a driver checks mock_core0_stalled() before dispatching. */
extern bool mock_one_shot;
extern bool mock_stall_model;
/* true: stamped at the read, as hal_common.c stamped before T11; false: at the
 * conversion, as it does since. */
extern bool mock_stamp_at_read;
extern uint32_t mock_stall_seed;
extern uint32_t mock_stall_count, mock_stall_total_ms, mock_stall_min_ms, mock_stall_max_ms;
extern uint32_t mock_stamp_lag_min_ms, mock_stamp_lag_max_ms; /* stamp minus conversion */
bool mock_core0_stalled(uint32_t now_ms);

/* ── Buzzer tone tracking ──────────────────────────────────────────── */
/* Counts hal_buzzer_tone_on/off calls so buzzer pattern tests can verify
 * the correct number of transitions without real hardware. */
extern int mock_buzzer_tone_on_count;
extern int mock_buzzer_tone_off_count;
/* Each change of the tone, with mock_time_ms, so a test can time a pattern. */
#define MOCK_BUZZER_EDGES 256
extern uint32_t mock_buzzer_edge_ms[MOCK_BUZZER_EDGES];
extern bool mock_buzzer_edge_on[MOCK_BUZZER_EDGES];
extern int mock_buzzer_edges;

/* The ground test pin: true is closed [GND-TEST-05]. */
extern bool mock_ground_test_pin;

void mock_reset_all(void);
/* What a restart loses: everything but the stored files. */
void mock_power_cycle(void);

/* How often the sensor has been initialised [SNS-REC-01]. */
extern int mock_pressure_inits;

/* The storage did not come up [FLT-BOOT-14]. */
extern bool mock_fs_unusable;

/* Whole-file writes, so a test can prove something is written once. */
extern uint32_t mock_fs_write_count;

#endif
