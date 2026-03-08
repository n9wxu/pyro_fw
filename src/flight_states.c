/*
 * Flight state machine: boot sequence + flight states.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "flight_states.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (ctx->buf_count == 4096) {
        if (ctx->apogee_protected && ctx->buf_tail == ctx->apogee_protect_idx) return;
        ctx->buf_tail = (ctx->buf_tail + 1) % 4096;
        ctx->buf_count--;
    }
    flight_sample_t *s = &ctx->flight_buffer[ctx->buf_head];
    s->time_ms = time_ms;
    s->pressure_pa = pressure;
    s->altitude_cm = altitude;
    s->state = st;
    s->under_thrust = 0;
    s->event = EVT_NONE;
    s->event_data = 0;
    ctx->buf_head = (ctx->buf_head + 1) % 4096;
    ctx->buf_count++;
}

static void buf_tag_event(flight_context_t *ctx, uint8_t event) {
    uint16_t last = (ctx->buf_head - 1 + 4096) % 4096;
    ctx->flight_buffer[last].event = event;
}

int32_t filter_pressure(flight_context_t *ctx, int32_t raw_pressure, uint32_t dt_ms) {
    if (!ctx->filter_initialized) {
        ctx->filtered_pressure = raw_pressure;
        ctx->filter_initialized = true;
        return raw_pressure;
    }
    int32_t diff = raw_pressure - ctx->filtered_pressure;
    int32_t alpha = (dt_ms * 1000) / (1000 + dt_ms);
    int32_t step = (diff * alpha) / 1000;
    if (step == 0 && diff != 0) step = (diff > 0) ? 1 : -1;
    ctx->filtered_pressure += step;
    return ctx->filtered_pressure;
}

#define MAX_ALTITUDE_CM 800000

int32_t pressure_to_altitude_cm(int32_t pressure_pa, int32_t ground_pressure_pa) {
    int32_t alt = ((ground_pressure_pa - pressure_pa) * 83) / 10;
    if (alt > MAX_ALTITUDE_CM) alt = MAX_ALTITUDE_CM;
    if (alt < 0) alt = 0;
    return alt;
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
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    int32_t clamped = ((int32_t)value > max_units) ? max_units : (int32_t)value;
    int32_t fallen = cm_to_units(ctx->max_altitude - ctx->last_altitude, ctx->config.units);
    int32_t agl = cm_to_units(ctx->last_altitude, ctx->config.units);
    int32_t speed = cm_to_units(-ctx->vertical_speed_cms, ctx->config.units);
    uint32_t delay_s = (hal_time_ms() - ctx->apogee_time) / 1000;
    switch (mode) {
        case PYRO_MODE_FALLEN: return fallen >= clamped;
        case PYRO_MODE_AGL:    return agl <= clamped;
        case PYRO_MODE_SPEED:  return speed >= clamped;
        case PYRO_MODE_DELAY:  return delay_s >= value;
        default: return false;
    }
}

/* ── Config parser ─────────────────────────────────────────────────── */

static uint8_t parse_mode(const char *val) {
    if (strcmp(val, "delay") == 0) return PYRO_MODE_DELAY;
    if (strcmp(val, "agl") == 0)   return PYRO_MODE_AGL;
    if (strcmp(val, "fallen") == 0) return PYRO_MODE_FALLEN;
    if (strcmp(val, "speed") == 0) return PYRO_MODE_SPEED;
    return 0;
}

void parse_config_ini(char *buf, config_t *cfg) {
    char *line = buf;
    while (line && *line) {
        char *nl = strstr(line, "\r\n");
        if (nl) { *nl = '\0'; nl += 2; }
        else { char *nl2 = strchr(line, '\n'); if (nl2) { *nl2 = '\0'; nl = nl2 + 1; } else nl = NULL; }
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *key = line, *val = eq + 1;
            if (strcmp(key, "id") == 0)          { strncpy(cfg->id, val, 8); cfg->id[8] = '\0'; }
            else if (strcmp(key, "name") == 0)    { strncpy(cfg->name, val, 8); cfg->name[8] = '\0'; }
            else if (strcmp(key, "pyro1_mode") == 0)  cfg->pyro1_mode = parse_mode(val);
            else if (strcmp(key, "pyro1_value") == 0) cfg->pyro1_value = (uint16_t)atoi(val);
            else if (strcmp(key, "pyro2_mode") == 0)  cfg->pyro2_mode = parse_mode(val);
            else if (strcmp(key, "pyro2_value") == 0) cfg->pyro2_value = (uint16_t)atoi(val);
            else if (strcmp(key, "units") == 0) {
                if (strcmp(val, "m") == 0) cfg->units = 1;
                else if (strcmp(val, "ft") == 0) cfg->units = 2;
                else cfg->units = 0;
            }
        }
        line = nl;
    }
}

/* ── Boot states ──────────────────────────────────────────────────── */

static const char *DEFAULT_CONFIG =
    "[pyro]\r\n"
    "id=PYRO001\r\n"
    "name=My Rocket\r\n"
    "pyro1_mode=delay\r\n"
    "pyro1_value=0\r\n"
    "pyro2_mode=agl\r\n"
    "pyro2_value=300\r\n"
    "units=m\r\n"
    "beep_mode=digits\r\n";

flight_state_t state_boot_filesystem(flight_context_t *ctx, uint32_t now) {
    (void)now;
    char buf[256];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n < 0) {
        /* File missing or error — create default */
        hal_fs_write_file("config.ini", DEFAULT_CONFIG, strlen(DEFAULT_CONFIG));
    } else if (n > 0) {
        buf[n] = '\0';
        parse_config_ini(buf, &ctx->config);
    }

    hal_buzzer_init();
    ctx->boot_timer = hal_time_ms();
    return BOOT_I2C_SETTLE;
}

flight_state_t state_boot_i2c_settle(flight_context_t *ctx, uint32_t now) {
    return (now - ctx->boot_timer >= 500) ? BOOT_SENSOR_DETECT : BOOT_I2C_SETTLE;
}

flight_state_t state_boot_sensor_detect(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    hal_pressure_init();
    return BOOT_PYRO_INIT;
}

flight_state_t state_boot_pyro_init(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    hal_pyro_init();
    return BOOT_CONTINUITY;
}

flight_state_t state_boot_continuity(flight_context_t *ctx, uint32_t now) {
    hal_continuity_t c1, c2;
    hal_pyro_check(&c1, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
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
        hal_pressure_t data;
        hal_pressure_read(&data);
        ctx->cal_sum += data.pressure_pa;
        ctx->cal_count++;
        if (ctx->cal_count >= 10) {
            ctx->ground_pressure = ctx->cal_sum / 10;
            return BOOT_MDNS;
        }
    }
    return BOOT_CALIBRATE;
}

flight_state_t state_boot_mdns(flight_context_t *ctx, uint32_t now) {
    (void)ctx; (void)now;
    return PAD_IDLE;
}

/* ── Flight states ────────────────────────────────────────────────── */

flight_state_t state_pad_idle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 10) return PAD_IDLE;

    if (now - ctx->last_cont_check > 1000) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        ctx->pyro1_continuity_good = c1.good;
        ctx->pyro2_continuity_good = c2.good;
        ctx->pyro1_adc = c1.raw_adc;
        ctx->pyro2_adc = c2.raw_adc;
        ctx->last_cont_check = now;

        if (!ctx->buzzer_started) {
            ctx->buzzer_started = true;
            int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
            bool p1_over = (ctx->config.pyro1_mode != PYRO_MODE_DELAY &&
                            ctx->config.pyro1_value > max_units);
            bool p2_over = (ctx->config.pyro2_mode != PYRO_MODE_DELAY &&
                            ctx->config.pyro2_value > max_units);
            uint8_t code = BEEP_ALL_GOOD;
            if (p1_over || p2_over) code = BEEP_CFG_RANGE;
            else if (!c1.good) code = c1.open ? BEEP_P1_OPEN : BEEP_P1_SHORT;
            else if (!c2.good) code = c2.open ? BEEP_P2_OPEN : BEEP_P2_SHORT;
            buzzer_set_code(code, true);
        }
    }

    hal_pressure_t pdata;
    hal_pressure_read(&pdata);
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
        buf_tag_event(ctx, EVT_LAUNCH);
        return ASCENT;
    }
    return PAD_IDLE;
}

flight_state_t state_ascent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return ASCENT;

    hal_pressure_t pdata;
    hal_pressure_read(&pdata);
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

    if (!ctx->pyros_armed && ctx->vertical_speed_cms < 1000 && ctx->vertical_speed_cms >= 0) {
        ctx->pyros_armed = true;
        buf_tag_event(ctx, EVT_ARMED);
    }

    if (ctx->pyros_armed && !ctx->apogee_detected && ctx->vertical_speed_cms <= 0) {
        ctx->apogee_detected = true;
        ctx->apogee_time = now;
        buf_tag_event(ctx, EVT_APOGEE);
        ctx->apogee_protect_idx = (ctx->buf_head >= 50) ? (ctx->buf_head - 50) : (4096 + ctx->buf_head - 50);
        ctx->apogee_protected = true;

        if (!ctx->pyro1_fired && !hal_pyro_is_firing() && ctx->pyro1_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
            hal_pyro_fire(1); ctx->pyro1_fired = true; ctx->pyro1_fire_time = now;
            buf_tag_event(ctx, EVT_PYRO1_FIRE);
        }
        if (!ctx->pyro2_fired && !hal_pyro_is_firing() && ctx->pyro2_continuity_good &&
            should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
            hal_pyro_fire(2); ctx->pyro2_fired = true; ctx->pyro2_fire_time = now;
            buf_tag_event(ctx, EVT_PYRO2_FIRE);
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

    hal_pressure_t pdata;
    hal_pressure_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0)
        ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, DESCENT);

    if (!ctx->pyro1_fired && !hal_pyro_is_firing() && ctx->pyro1_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro1_mode, ctx->config.pyro1_value)) {
        hal_pyro_fire(1); ctx->pyro1_fired = true; ctx->pyro1_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO1_FIRE);
    }
    if (!ctx->pyro2_fired && !hal_pyro_is_firing() && ctx->pyro2_continuity_good &&
        should_fire_pyro(ctx, ctx->config.pyro2_mode, ctx->config.pyro2_value)) {
        hal_pyro_fire(2); ctx->pyro2_fired = true; ctx->pyro2_fire_time = now;
        buf_tag_event(ctx, EVT_PYRO2_FIRE);
    }

    if (ctx->pyro1_fired && ctx->pyro1_fire_time > 0 && now - ctx->pyro1_fire_time > 1000 && now - ctx->pyro1_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c1.open) { hal_pyro_fire(1); ctx->pyro1_fire_time = now; }
    }
    if (ctx->pyro2_fired && ctx->pyro2_fire_time > 0 && now - ctx->pyro2_fire_time > 1000 && now - ctx->pyro2_fire_time < 1500) {
        bool ballistic = ctx->vertical_speed_cms < -3000;
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c2.open) { hal_pyro_fire(2); ctx->pyro2_fire_time = now; }
    }

    if (abs(altitude - ctx->last_altitude) < 100 &&
        abs(ctx->vertical_speed_cms) < 200 &&
        altitude < 3000) {
        if (ctx->landing_stable_since == 0) ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            buf_tag_event(ctx, EVT_LANDING);
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
    if (!ctx->landed_beep_started) {
        ctx->landed_beep_started = true;
        int32_t alt = cm_to_units(ctx->max_altitude, ctx->config.units);
        buzzer_set_altitude(alt);
    }

    if (now - ctx->last_sample < 1000) return LANDED;

    hal_pressure_t pdata;
    hal_pressure_read(&pdata);
    uint32_t dt = now - ctx->last_sample;
    int32_t filtered = filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt);
    int32_t altitude = pressure_to_altitude_cm(filtered, ctx->ground_pressure);

    uint32_t flight_time = now - ctx->launch_time;
    buf_add(ctx, flight_time, filtered, altitude, LANDED);

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return LANDED;
}

/* ── Flight init and output ────────────────────────────────────────── */

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->config.id, "PYRO001", 8);
    strncpy(ctx->config.name, "PYRO001", 8);
    ctx->config.pyro1_mode = PYRO_MODE_DELAY;
    ctx->config.pyro1_value = 0;
    ctx->config.pyro2_mode = PYRO_MODE_AGL;
    ctx->config.pyro2_value = 300;
    ctx->config.units = 1;
    ctx->current_state = BOOT_FILESYSTEM;
}

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    buzzer_update(now);

    if (ctx->current_state >= PAD_IDLE) {
        uint32_t interval = (ctx->current_state == ASCENT || ctx->current_state == DESCENT) ? 100 : 1000;
        if (now - ctx->last_telemetry >= interval) {
            uint32_t ft = (ctx->current_state != PAD_IDLE) ? (now - ctx->launch_time) : 0;
            send_telemetry(ctx, ft, ctx->last_altitude, ctx->current_state);
            ctx->last_telemetry = now;
        }
    }
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
