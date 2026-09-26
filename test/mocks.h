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
    int fire_count;
    uint8_t last_fire_channel;
    /* The board takes the call and energises nothing, as MK1C does while its
     * firing sequence is unimplemented. Counted separately from fire_count,
     * which counts channels that were actually energised. */
    bool refuse_fire;
    int refused_count;
    int sample_count; /* hal_pyro_sample() calls; one shared stimulus each */
} mock_pyro_t;

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

/* ── Sensor model ─────────────────────────────────────────────────── */
/* Off by default: a test that sets none of these sees the smooth signal it
 * always has. Every reading passes the HAL's own range check, and is
 * truncated to whole pascals as hal_common.c does. */
extern uint32_t mock_sample_interval_ms; /* 20: the MS5607 at 50 Hz */
extern float mock_noise_rms_pa;          /* Gaussian noise on every reading */
extern uint32_t mock_noise_seed;         /* the same seed gives the same noise */
extern int32_t mock_glitch_pa;           /* added to each of the next ... */
extern int mock_glitch_samples;          /* ... this many readings */
extern uint32_t mock_pres_rejects;

/* The MS5607's timing on the hardware, with core0 stalls. A reading is the
 * pressure at the middle of its D1 conversion, stamped when the loop reads it
 * after D2 -- about 16 ms later, or later still by any stall in between.
 * Stalls last 40-73 ms and come about 1.3 times a second (DD-035). While one
 * lasts nothing on core0 runs: a driver checks mock_core0_stalled() before
 * dispatching. */
extern bool mock_stall_model;
extern uint32_t mock_stall_seed;
extern uint32_t mock_stall_count, mock_stall_total_ms, mock_stall_min_ms, mock_stall_max_ms;
extern uint32_t mock_stamp_lag_min_ms, mock_stamp_lag_max_ms; /* stamp minus conversion */
bool mock_core0_stalled(uint32_t now_ms);

/* ── Serial command mock [GND-TEST-01..04, DD-011] ─────────────────── */
/* Queue serial command strings to be returned by hal_serial_readline().
 * Each enqueued line is returned once on the next hal_serial_readline()
 * call; subsequent calls return false until another line is enqueued. */
#define MOCK_SERIAL_QUEUE_DEPTH 8
#define MOCK_SERIAL_LINE_MAX 64

extern char mock_serial_queue[MOCK_SERIAL_QUEUE_DEPTH][MOCK_SERIAL_LINE_MAX];
extern int mock_serial_queue_count;

/* Enqueue a command string (NUL-terminated, no CR/LF needed) */
void mock_serial_enqueue(const char *cmd);

/* ── Buzzer tone tracking ──────────────────────────────────────────── */
/* Counts hal_buzzer_tone_on/off calls so buzzer pattern tests can verify
 * the correct number of transitions without real hardware. */
extern int mock_buzzer_tone_on_count;
extern int mock_buzzer_tone_off_count;

void mock_reset_all(void);

/* Whole-file writes, so a test can prove something is written once. */
extern uint32_t mock_fs_write_count;

#endif
