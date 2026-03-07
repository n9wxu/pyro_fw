/*
 * Flight state machine: boot sequence + flight states.
 * SPDX-License-Identifier: MIT
 */
#ifdef UNIT_TEST
#include "mocks.h"
#else
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <lfs.h>
#include <pico_fota_bootloader/core.h>
#endif

#include "flight_states.h"
#include "pressure_sensor.h"
#include "pyro.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNIT_TEST
extern const struct lfs_config lfs_pico_flash_config;

/* Network functions (defined in net_glue.c / http_server.c) */
void net_init(void);
void net_start(void);
void net_mdns_poll(void);
void http_server_init(void);
#endif

#define BUZZER 16

/* ── Helpers ──────────────────────────────────────────────────────── */

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (ctx->buf_count == 4096) {
        if (ctx->apogee_protected && ctx->buf_tail == ctx->apogee_protect_idx) return;
        ctx->buf_tail = (ctx->buf_tail + 1) % 4096;
        ctx->buf_count--;
    }
    ctx->flight_buffer[ctx->buf_head].time_ms = time_ms;
    ctx->flight_buffer[ctx->buf_head].pressure_pa = pressure;
    ctx->flight_buffer[ctx->buf_head].altitude_cm = altitude;
    ctx->flight_buffer[ctx->buf_head].state = st;
    ctx->buf_head = (ctx->buf_head + 1) % 4096;
    ctx->buf_count++;
}

int32_t filter_pressure(flight_context_t *ctx, int32_t raw_pressure, uint32_t dt_ms) {
    if (!ctx->filter_initialized) {
        ctx->filtered_pressure = raw_pressure;
        ctx->filter_initialized = true;
        return raw_pressure;
    }
    int32_t alpha = (dt_ms * 1000) / (1000 + dt_ms);
    ctx->filtered_pressure += ((raw_pressure - ctx->filtered_pressure) * alpha) / 1000;
    return ctx->filtered_pressure;
}

int32_t pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    return ((ground_pressure_pa - pressure_pa) * 83) / 10;
}

static int32_t cm_to_units(int32_t cm, uint8_t units) {
    switch (units) {
        case 1:  return cm / 100;
        case 2:  return cm * 100 / 3048;
        default: return cm;
    }
}

bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected) return false;
    int32_t fallen = cm_to_units(ctx->max_altitude - ctx->last_altitude, ctx->config.units);
    int32_t agl = cm_to_units(ctx->last_altitude, ctx->config.units);
    int32_t speed = cm_to_units(-ctx->vertical_speed_cms, ctx->config.units);
    uint32_t delay_s = (to_ms_since_boot(get_absolute_time()) - ctx->apogee_time) / 1000;
    switch (mode) {
        case PYRO_MODE_FALLEN: return fallen >= value;
        case PYRO_MODE_AGL:    return agl <= (int32_t)value;
        case PYRO_MODE_SPEED:  return speed >= (int32_t)value;
        case PYRO_MODE_DELAY:  return delay_s >= value;
        default: return false;
    }
}

/* ── Boot states ──────────────────────────────────────────────────── */

flight_state_t state_boot_filesystem(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    lfs_t lfs;
    int err = lfs_mount(&lfs, &lfs_pico_flash_config);
    if (err < 0) {
        lfs_format(&lfs, &lfs_pico_flash_config);
        lfs_mount(&lfs, &lfs_pico_flash_config);
    }
    uart_puts(uart0, "littlefs mounted\r\n");
    lfs_file_t config_file;
    err = lfs_file_open(&lfs, &config_file, "config.ini", LFS_O_RDONLY);
    if (err == LFS_ERR_NOENT) {
        err = lfs_file_open(&lfs, &config_file, "config.ini", LFS_O_WRONLY | LFS_O_CREAT);
        if (err == LFS_ERR_OK) {
            const char *config =
                "[pyro]\r\n"
                "id=PYRO001\r\n"
                "name=My Rocket\r\n"
                "pyro1_mode=delay\r\n"
                "pyro1_value=0\r\n"
                "pyro2_mode=agl\r\n"
                "pyro2_value=300\r\n"
                "units=m\r\n"
                "beep_mode=digits\r\n";
            lfs_file_write(&lfs, &config_file, config, strlen(config));
            lfs_file_close(&lfs, &config_file);
        }
    } else if (err == LFS_ERR_OK) {
        lfs_file_close(&lfs, &config_file);
    }
    lfs_unmount(&lfs);

    net_init();
    net_start();
    http_server_init();
    uart_puts(uart0, "network ready\r\n");

    pfb_firmware_commit();

    adc_gpio_init(26);
    adc_gpio_init(27);
    gpio_init(BUZZER);
    gpio_set_dir(BUZZER, GPIO_OUT);
    gpio_put(BUZZER, 0);
    buzzer_init();

    uart_puts(uart0, "I2C init...\r\n");
    i2c_deinit(i2c1);

    ctx->boot_timer = to_ms_since_boot(get_absolute_time());
    return BOOT_I2C_SETTLE;
}

flight_state_t state_boot_i2c_settle(flight_context_t *ctx, uint32_t now) {
    return (now - ctx->boot_timer >= 500) ? BOOT_SENSOR_DETECT : BOOT_I2C_SETTLE;
}

flight_state_t state_boot_sensor_detect(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    pressure_sensor_type_t sensor = pressure_sensor_init();
    if (sensor == PRESSURE_SENSOR_NONE)
        uart_puts(uart0, "No pressure sensor!\r\n");
    else {
        uart_puts(uart0, pressure_sensor_name());
        uart_puts(uart0, " detected\r\n");
    }
    return BOOT_PYRO_INIT;
}

flight_state_t state_boot_pyro_init(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    pyro_init();
    uart_puts(uart0, "pyro init done\r\n");
    return BOOT_CONTINUITY;
}

flight_state_t state_boot_continuity(flight_context_t *ctx, uint32_t now) {
    pyro_continuity_t cont1, cont2;
    pyro_check_continuity(&cont1, &cont2);
    ctx->pyro1_continuity_good = cont1.good;
    ctx->pyro2_continuity_good = cont2.good;
    uart_puts(uart0, "calibrating...\r\n");
    ctx->boot_timer = now;
    return BOOT_STABILIZE;
}

flight_state_t state_boot_stabilize(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer >= 2000) {
        ctx->cal_count = 0;
        ctx->cal_sum = 0;
        ctx->boot_timer = now;
        return BOOT_CALIBRATE;
    }
    return BOOT_STABILIZE;
}

flight_state_t state_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer >= 100) {
        ctx->boot_timer = now;
        pressure_reading_t data;
        pressure_sensor_read(&data);
        ctx->cal_sum += data.pressure_pa;
        ctx->cal_count++;
        if (ctx->cal_count >= 10) {
            ctx->ground_pressure = ctx->cal_sum / 10;
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "ground_pressure=%ld\r\n", (long)ctx->ground_pressure);
            uart_puts(uart0, dbg);
            return BOOT_MDNS;
        }
    }
    return BOOT_CALIBRATE;
}

flight_state_t state_boot_mdns(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    uart_puts(uart0, "starting mDNS...\r\n");
    net_mdns_poll();
    uart_puts(uart0, "mDNS started\r\n");
    uart_puts(uart0, "entering PAD_IDLE\r\n");
    return PAD_IDLE;
}

/* ── Flight states ────────────────────────────────────────────────── */

flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 10) return PAD_IDLE;

    if (now - ctx->last_cont_check > 1000) {
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        ctx->pyro1_continuity_good = c1.good;
        ctx->pyro2_continuity_good = c2.good;
        ctx->pyro1_adc = c1.raw_adc;
        ctx->pyro2_adc = c2.raw_adc;
        ctx->last_cont_check = now;

        /* Update beep code based on continuity */
        uint8_t code = BEEP_ALL_GOOD;
        if (!c1.good) code = c1.open ? BEEP_P1_OPEN : BEEP_P1_SHORT;
        else if (!c2.good) code = c2.open ? BEEP_P2_OPEN : BEEP_P2_SHORT;
        if (!buzzer_is_active())
            buzzer_set_code(code, true);
    }

    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = (ctx->last_sample > 0) ? (now - ctx->last_sample) : 10;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    buf_add(ctx, 0, filtered, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    if (altitude > 1000) {
        buzzer_stop();
        ctx->launch_time = now;
        for (int i = ctx->buf_count - 1; i >= 0; i--) {
            uint16_t idx = (ctx->buf_head - 1 - i + 4096) % 4096;
            if (ctx->flight_buffer[idx].altitude_cm <= 50) {
                ctx->launch_time = now - (ctx->buf_count - 1 - i) * 10;
                break;
            }
        }
        return ASCENT;
    }
    return PAD_IDLE;
}

flight_state_t state_ascent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return ASCENT;

    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;

    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, ASCENT);
    uint16_t last_idx = (ctx->buf_head - 1 + 4096) % 4096;
    ctx->flight_buffer[last_idx].under_thrust = ctx->under_thrust ? 1 : 0;

    if (altitude > ctx->max_altitude)
        ctx->max_altitude = altitude;

    if (!ctx->pyros_armed && ctx->vertical_speed_cms < 1000 && ctx->vertical_speed_cms >= 0)
        ctx->pyros_armed = true;

    if (ctx->pyros_armed && !ctx->apogee_detected && ctx->vertical_speed_cms <= 0) {
        ctx->apogee_detected = true;
        ctx->apogee_time = now;
        ctx->apogee_protect_idx = (ctx->buf_head >= 50) ? (ctx->buf_head - 50) : (4096 + ctx->buf_head - 50);
        ctx->apogee_protected = true;

        if (!ctx->pyro1_fired && !pyro_is_firing() && ctx->pyro1_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
            pyro_fire(1); ctx->pyro1_fire_time = now;
        }
        if (!ctx->pyro2_fired && !pyro_is_firing() && ctx->pyro2_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
            pyro_fire(2); ctx->pyro2_fire_time = now;
        }

        ctx->last_altitude = altitude;
        ctx->last_sample = now;
        return DESCENT;
    }

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return ASCENT;
}

flight_state_t state_descent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 50) return DESCENT;

    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, DESCENT);

    if (!ctx->pyro1_fired && !pyro_is_firing() && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
        pyro_fire(1); ctx->pyro1_fire_time = now;
    }
    if (!ctx->pyro2_fired && !pyro_is_firing() && ctx->pyro2_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
        pyro_fire(2); ctx->pyro2_fire_time = now;
    }

    if (ctx->pyro1_fired && ctx->pyro1_fire_time > 0 && now - ctx->pyro1_fire_time > 1000 && now - ctx->pyro1_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        if (ballistic && !c1.open) { pyro_fire(1); ctx->pyro1_fire_time = now; }
    }
    if (ctx->pyro2_fired && ctx->pyro2_fire_time > 0 && now - ctx->pyro2_fire_time > 1000 && now - ctx->pyro2_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        pyro_continuity_t c1, c2;
        pyro_check_continuity(&c1, &c2);
        if (ballistic && !c2.open) { pyro_fire(2); ctx->pyro2_fire_time = now; }
    }

    if (abs(altitude - ctx->last_altitude) < 100) {
        if (ctx->landing_stable_since == 0) ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            ctx->last_altitude = altitude;
            ctx->last_sample = now;
            return LANDED;
        }
    } else {
        ctx->landing_stable_since = 0;
    }

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return DESCENT;
}

flight_state_t state_landed(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 1000) return LANDED;

    pressure_reading_t pdata;
    pressure_sensor_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, LANDED);

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return LANDED;
}

/* ── Dispatch ─────────────────────────────────────────────────────── */

flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    switch (ctx->current_state) {
        case BOOT_FILESYSTEM:    return state_boot_filesystem(ctx, now);
        case BOOT_I2C_SETTLE:    return state_boot_i2c_settle(ctx, now);
        case BOOT_SENSOR_DETECT: return state_boot_sensor_detect(ctx, now);
        case BOOT_PYRO_INIT:     return state_boot_pyro_init(ctx, now);
        case BOOT_CONTINUITY:    return state_boot_continuity(ctx, now);
        case BOOT_STABILIZE:     return state_boot_stabilize(ctx, now);
        case BOOT_CALIBRATE:     return state_boot_calibrate(ctx, now);
        case BOOT_MDNS:          return state_boot_mdns(ctx, now);
        case PAD_IDLE: return state_pad_idle(ctx, now);
        case ASCENT:   return state_ascent(ctx, now);
        case DESCENT:  return state_descent(ctx, now);
        case LANDED:   return state_landed(ctx, now);
        default:       return PAD_IDLE;
    }
}
