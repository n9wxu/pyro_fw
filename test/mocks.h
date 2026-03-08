/*
 * Mock hardware layer for unit tests.
 * Provides fake implementations of all hardware functions
 * called by flight_states.c and telemetry.c.
 */
#ifndef TEST_MOCKS_H
#define TEST_MOCKS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Mock control ─────────────────────────────────────────────────── */

/* Pressure sensor mock */
typedef struct {
    float pressure_pa;
    float temperature_c;
    int sensor_type;  /* 0=none, 1=ms5607, 2=bmp280 */
} mock_pressure_t;

extern mock_pressure_t mock_pressure;

/* Pyro mock */
typedef struct {
    uint16_t p1_adc;
    uint16_t p2_adc;
    bool p1_good;
    bool p2_good;
    bool p1_open;
    bool p2_open;
    bool firing;
    int fire_count;
    uint8_t last_fire_channel;
} mock_pyro_t;

extern mock_pyro_t mock_pyro;

/* UART capture */
#define MOCK_UART_BUF_SIZE 4096
extern char mock_uart_buf[MOCK_UART_BUF_SIZE];
extern int mock_uart_len;

/* Time mock */
extern uint32_t mock_time_ms;

void mock_reset_all(void);

/* ── Pico SDK type stubs ──────────────────────────────────────────── */

typedef int uart_inst_t;
extern uart_inst_t uart0_inst;
#define uart0 (&uart0_inst)

void uart_puts(uart_inst_t *uart, const char *s);

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) {
    extern uint32_t mock_time_ms;
    return (absolute_time_t)mock_time_ms * 1000;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000);
}

/* GPIO stubs */
#define GPIO_OUT 1
#define GPIO_FUNC_I2C 3
static inline void gpio_init(unsigned int pin) { (void)pin; }
static inline void gpio_set_dir(unsigned int pin, int dir) { (void)pin; (void)dir; }
static inline void gpio_put(unsigned int pin, int val) { (void)pin; (void)val; }
static inline void gpio_set_function(unsigned int pin, int fn) { (void)pin; (void)fn; }
static inline void gpio_pull_up(unsigned int pin) { (void)pin; }
static inline void gpio_disable_pulls(unsigned int pin) { (void)pin; }
static inline void adc_gpio_init(unsigned int pin) { (void)pin; }

/* I2C stubs */
typedef int i2c_inst_t;
extern i2c_inst_t i2c1_inst;
#define i2c1 (&i2c1_inst)
static inline void i2c_deinit(i2c_inst_t *i2c) { (void)i2c; }
static inline int i2c_init(i2c_inst_t *i2c, unsigned int baud) { (void)i2c; (void)baud; return 0; }

/* LFS stubs */
#define LFS_ERR_OK 0
#define LFS_ERR_NOENT (-2)
#define LFS_O_RDONLY 1
#define LFS_O_WRONLY 2
#define LFS_O_CREAT 0x0100
#define LFS_O_TRUNC 0x0200
#define LFS_ERR_NOENT (-2)
#define LFS_ERR_OK 0
typedef int lfs_ssize_t;
typedef struct { int dummy; } lfs_t;
typedef struct { int dummy; } lfs_file_t;
typedef struct { int block_count; int block_size; } lfs_config;
extern const lfs_config lfs_pico_flash_config;
static inline int lfs_mount(lfs_t *l, const lfs_config *c) { (void)l; (void)c; return 0; }
static inline int lfs_format(lfs_t *l, const lfs_config *c) { (void)l; (void)c; return 0; }
static inline int lfs_unmount(lfs_t *l) { (void)l; return 0; }
static inline int lfs_file_open(lfs_t *l, lfs_file_t *f, const char *p, int fl) { (void)l; (void)f; (void)p; (void)fl; return 0; }
static inline lfs_ssize_t lfs_file_read(lfs_t *l, lfs_file_t *f, void *b, size_t s) { (void)l; (void)f; (void)b; (void)s; return 0; }
static inline int lfs_file_write(lfs_t *l, lfs_file_t *f, const void *b, size_t s) { (void)l; (void)f; (void)b; return (int)s; }
static inline int lfs_file_close(lfs_t *l, lfs_file_t *f) { (void)l; (void)f; return 0; }

/* Network stubs */
static inline void net_init(void) {}
static inline void net_start(void) {}
static inline void net_mdns_poll(void) {}
static inline void http_server_init(void) {}

/* Bootloader stub */
static inline void pfb_firmware_commit(void) {}

#endif
