/*
 * HAL implementation for real Pico hardware.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "tusb.h"
#include "bsp/board_api.h"
#include <lfs.h>
#include <pico_fota_bootloader/core.h>
#include "pressure_sensor.h"
#include "pyro.h"

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

/* ── Time ─────────────────────────────────────────────────────────── */

uint32_t hal_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/* ── Pressure sensor ──────────────────────────────────────────────── */

int hal_pressure_init(void) {
    return (int)pressure_sensor_init();
}

bool hal_pressure_read(hal_pressure_t *out) {
    pressure_reading_t r;
    if (!pressure_sensor_read(&r)) return false;
    out->pressure_pa = r.pressure_pa;
    out->temperature_c = r.temperature_c;
    return true;
}

/* ── Pyro ─────────────────────────────────────────────────────────── */

void hal_pyro_init(void) { pyro_init(); }

void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2) {
    pyro_continuity_t c1, c2;
    pyro_check_continuity(&c1, &c2);
    p1->raw_adc = c1.raw_adc; p1->good = c1.good; p1->open = c1.open; p1->shorted = c1.shorted;
    p2->raw_adc = c2.raw_adc; p2->good = c2.good; p2->open = c2.open; p2->shorted = c2.shorted;
}

void hal_pyro_fire(uint8_t channel) { pyro_fire(channel); }
void hal_pyro_update(uint32_t now_ms) { pyro_update(now_ms); }
bool hal_pyro_is_firing(void) { return pyro_is_firing(); }

/* ── Buzzer ───────────────────────────────────────────────────────── */

void hal_buzzer_init(void) {
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);
}

void hal_buzzer_tone_on(void)  { gpio_put(BUZZER_PIN, 1); }
void hal_buzzer_tone_off(void) { gpio_put(BUZZER_PIN, 0); }

/* ── Telemetry ────────────────────────────────────────────────────── */

void hal_telemetry_send(const char *sentence) {
    uart_puts(uart0, sentence);
}

/* ── Filesystem ───────────────────────────────────────────────────── */

int hal_fs_mount(void) {
    lfs_t lfs;
    int err = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&lfs, &lfs_pico_flash_config);
        err = lfs_mount(&lfs, &lfs_pico_flash_config);
    }
    if (err == 0) lfs_unmount(&lfs);
    return err;
}

void hal_fs_unmount(void) {
    /* Each read/write mounts and unmounts internally */
}

int hal_fs_read_file(const char *path, char *buf, int max_len) {
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK) return -1;
    lfs_file_t f;
    int err = lfs_file_open(&lfs, &f, path, LFS_O_RDONLY);
    if (err == LFS_ERR_NOENT) { lfs_unmount(&lfs); return -2; }
    if (err != LFS_ERR_OK) { lfs_unmount(&lfs); return -1; }
    lfs_ssize_t n = lfs_file_read(&lfs, &f, buf, max_len);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    return (int)n;
}

int hal_fs_write_file(const char *path, const char *data, int len) {
    lfs_t lfs;
    if (lfs_mount(&lfs, &lfs_pico_flash_config) != LFS_ERR_OK) return -1;
    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
        lfs_unmount(&lfs); return -1;
    }
    lfs_file_write(&lfs, &f, data, len);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    return 0;
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
}

void hal_firmware_commit(void) {
    pfb_firmware_commit();
}
