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
} mock_pyro_t;

#define MOCK_UART_BUF_SIZE 4096

extern mock_pressure_t mock_pressure;
extern mock_pyro_t mock_pyro;
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

void mock_reset_all(void);

#endif
