/*
 * Flight state machine: event-driven with transition table.
 * See REQUIREMENTS.md for requirement definitions.
 * See TRACEABILITY.md for requirement-to-test mapping.
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "flight_states.h"
#include "buzzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ring buffer ──────────────────────────────────────────────────── */

void buf_add(flight_context_t *ctx, uint32_t time_ms, int32_t pressure, int32_t altitude, uint8_t st) {
    if (ctx->buf_count == 4096) {
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

static void buf_tag_event(flight_context_t *ctx, uint8_t event) { /* [DAT-03] */
    ctx->flight_buffer[(ctx->buf_head - 1 + 4096) % 4096].event = event;
}

/* ── Pressure processing ──────────────────────────────────────────── */

/* [SNS-PRES-02..04] First-order IIR with minimum step to prevent stall.
 * See IMPLEMENTATION.md "Pressure Filter" for derivation. */
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

/* [SNS-ALT-01..03] Linear barometric approximation, clamped.
 * See IMPLEMENTATION.md "Altitude Limitations" for accuracy vs altitude. */
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

static void read_and_filter_pressure(flight_context_t *ctx, uint32_t now, int32_t *altitude_out) {
    hal_pressure_t pdata;
    hal_pressure_read(&pdata);
    uint32_t dt = (ctx->last_sample > 0) ? (now - ctx->last_sample) : 10;
    *altitude_out = pressure_to_altitude_cm(
        filter_pressure(ctx, (int32_t)pdata.pressure_pa, dt),
        ctx->ground_pressure);
}

/* ── Pyro firing logic ────────────────────────────────────────────── */

/* [PYR-MODE-01..04, PYR-ALT-01] See IMPLEMENTATION.md "Altitude Limitations"
 * for behavior above 8000m. */
bool should_fire_pyro(flight_context_t *ctx, uint8_t mode, uint16_t value) {
    if (!ctx->apogee_detected) return false; /* [PYR-SAFE-04] */
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

/* [PYR-SAFE-01..03] */
static void try_fire_pyros(flight_context_t *ctx, uint32_t now) {
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
}

/* [PYR-REFIRE-01] Re-attempt if still ballistic 1-1.5s after first fire */
static void check_refire(flight_context_t *ctx, uint32_t now) {
    bool ballistic = ctx->vertical_speed_cms < -3000;
    if (ctx->pyro1_fired && ctx->pyro1_fire_time > 0 &&
        now - ctx->pyro1_fire_time > 1000 && now - ctx->pyro1_fire_time < 1500) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c1.open) { hal_pyro_fire(1); ctx->pyro1_fire_time = now; }
    }
    if (ctx->pyro2_fired && ctx->pyro2_fire_time > 0 &&
        now - ctx->pyro2_fire_time > 1000 && now - ctx->pyro2_fire_time < 1500) {
        hal_continuity_t c1, c2;
        hal_pyro_check(&c1, &c2);
        if (ballistic && !c2.open) { hal_pyro_fire(2); ctx->pyro2_fire_time = now; }
    }
}

/* ── Config parser ────────────────────────────────────────────────── */

static uint8_t parse_mode(const char *val) {
    if (strcmp(val, "delay") == 0) return PYRO_MODE_DELAY;
    if (strcmp(val, "agl") == 0)   return PYRO_MODE_AGL;
    if (strcmp(val, "fallen") == 0) return PYRO_MODE_FALLEN;
    if (strcmp(val, "speed") == 0) return PYRO_MODE_SPEED;
    return 0;
}

static uint8_t parse_units(const char *val) {
    if (strcmp(val, "m") == 0) return 1;
    if (strcmp(val, "ft") == 0) return 2;
    return 0;
}

/* [CFG-02, CFG-06..09] */
static void parse_config_line(const char *key, const char *val, config_t *cfg) {
    if (strcmp(key, "id") == 0)               { strncpy(cfg->id, val, 8); cfg->id[8] = '\0'; }
    else if (strcmp(key, "name") == 0)         { strncpy(cfg->name, val, 8); cfg->name[8] = '\0'; }
    else if (strcmp(key, "pyro1_mode") == 0)   cfg->pyro1_mode = parse_mode(val);
    else if (strcmp(key, "pyro1_value") == 0)  cfg->pyro1_value = (uint16_t)atoi(val);
    else if (strcmp(key, "pyro2_mode") == 0)   cfg->pyro2_mode = parse_mode(val);
    else if (strcmp(key, "pyro2_value") == 0)  cfg->pyro2_value = (uint16_t)atoi(val);
    else if (strcmp(key, "units") == 0)        cfg->units = parse_units(val);
}

void parse_config_ini(char *buf, config_t *cfg) {
    char *line = buf;
    while (line && *line) {
        char *nl = strstr(line, "\r\n");
        if (nl) { *nl = '\0'; nl += 2; }
        else { char *nl2 = strchr(line, '\n'); if (nl2) { *nl2 = '\0'; nl = nl2 + 1; } else nl = NULL; }
        char *eq = strchr(line, '=');
        if (eq) { *eq = '\0'; parse_config_line(line, eq + 1, cfg); }
        line = nl;
    }
}

/* ── Event detectors ──────────────────────────────────────────────── */

static const char *DEFAULT_CONFIG =
    "[pyro]\r\nid=PYRO001\r\nname=My Rocket\r\n"
    "pyro1_mode=delay\r\npyro1_value=0\r\n"
    "pyro2_mode=agl\r\npyro2_value=300\r\n"
    "units=m\r\nbeep_mode=digits\r\n";

/* [FLT-BOOT-02, FLT-BOOT-03, CFG-05] */
static state_event_t detect_boot_init(flight_context_t *ctx, uint32_t now) {
    (void)now;
    char buf[256];
    int n = hal_fs_read_file("config.ini", buf, sizeof(buf) - 1);
    if (n < 0) hal_fs_write_file("config.ini", DEFAULT_CONFIG, strlen(DEFAULT_CONFIG));
    else if (n > 0) { buf[n] = '\0'; parse_config_ini(buf, &ctx->config); }
    hal_buzzer_init();
    hal_pressure_init();
    hal_pyro_init();
    ctx->boot_timer = hal_time_ms();
    return SEVT_DONE;
}

/* [FLT-BOOT-04, FLT-BOOT-09] */
static state_event_t detect_boot_settle(flight_context_t *ctx, uint32_t now) {
    return (now - ctx->boot_timer >= 2500) ? SEVT_TIMER : SEVT_NONE;
}

/* [SYS-STATUS-02] */
static state_event_t detect_boot_continuity(flight_context_t *ctx, uint32_t now) {
    (void)now;
    hal_continuity_t c1, c2;
    hal_pyro_check(&c1, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->boot_timer = now;
    return SEVT_DONE;
}

/* [FLT-BOOT-08] */
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->boot_timer < 100) return SEVT_NONE;
    ctx->boot_timer = now;
    hal_pressure_t data;
    hal_pressure_read(&data);
    ctx->cal_sum += data.pressure_pa;
    ctx->cal_count++;
    return (ctx->cal_count >= 10) ? SEVT_CAL_DONE : SEVT_NONE;
}

static void update_continuity_and_buzzer(flight_context_t *ctx, uint32_t now) { /* [PYR-CONT-01, PYR-ALT-02] */
    if (now - ctx->last_cont_check <= 1000) return;
    hal_continuity_t c1, c2;
    hal_pyro_check(&c1, &c2);
    ctx->pyro1_continuity_good = c1.good;
    ctx->pyro2_continuity_good = c2.good;
    ctx->pyro1_adc = c1.raw_adc;
    ctx->pyro2_adc = c2.raw_adc;
    ctx->last_cont_check = now;

    if (ctx->buzzer_started) return;
    ctx->buzzer_started = true;
    int32_t max_units = cm_to_units(MAX_ALTITUDE_CM, ctx->config.units);
    bool p1_over = (ctx->config.pyro1_mode != PYRO_MODE_DELAY && ctx->config.pyro1_value > max_units);
    bool p2_over = (ctx->config.pyro2_mode != PYRO_MODE_DELAY && ctx->config.pyro2_value > max_units);
    uint8_t code = BEEP_ALL_GOOD;
    if (p1_over || p2_over) code = BEEP_CFG_RANGE;
    else if (!c1.good) code = c1.open ? BEEP_P1_OPEN : BEEP_P1_SHORT;
    else if (!c2.good) code = c2.open ? BEEP_P2_OPEN : BEEP_P2_SHORT;
    buzzer_set_code(code, true);
}

/* [FLT-LAUNCH-01, FLT-LAUNCH-02, FLT-RATE-01] */
static state_event_t detect_pad_idle(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 10) return SEVT_NONE;

    update_continuity_and_buzzer(ctx, now);

    int32_t altitude;
    read_and_filter_pressure(ctx, now, &altitude);
    buf_add(ctx, 0, ctx->filtered_pressure, altitude, PAD_IDLE);
    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    return (altitude > 1000) ? SEVT_LAUNCH : SEVT_NONE;
}

/* [FLT-ASC-01..06, FLT-APO-01..04, FLT-RATE-02] */
static state_event_t detect_ascent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 100) return SEVT_NONE;

    int32_t altitude;
    read_and_filter_pressure(ctx, now, &altitude);
    uint32_t dt = now - ctx->last_sample;

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0) ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;
    ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, ASCENT);
    ctx->flight_buffer[(ctx->buf_head - 1 + 4096) % 4096].under_thrust = ctx->under_thrust ? 1 : 0;

    if (altitude > ctx->max_altitude) ctx->max_altitude = altitude;
    ctx->last_altitude = altitude;
    ctx->last_sample = now;

    if (!ctx->pyros_armed && ctx->vertical_speed_cms < 1000 && ctx->vertical_speed_cms >= 0)
        return SEVT_ARMED;
    if (ctx->pyros_armed && !ctx->apogee_detected && ctx->vertical_speed_cms <= 0)
        return SEVT_APOGEE;
    return SEVT_NONE;
}

/* [FLT-LAND-01..03, FLT-RATE-03] */
static state_event_t detect_descent(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 50) return SEVT_NONE;

    int32_t altitude;
    read_and_filter_pressure(ctx, now, &altitude);
    uint32_t dt = now - ctx->last_sample;

    ctx->prev_vertical_speed_cms = ctx->vertical_speed_cms;
    if (dt > 0) ctx->vertical_speed_cms = (altitude - ctx->last_altitude) * 1000 / (int32_t)dt;

    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, DESCENT);
    try_fire_pyros(ctx, now);
    check_refire(ctx, now);

    bool altitude_stable = abs(altitude - ctx->last_altitude) < 100;
    bool speed_low = abs(ctx->vertical_speed_cms) < 200;
    bool near_ground = altitude < 3000;

    if (altitude_stable && speed_low && near_ground) {
        if (ctx->landing_stable_since == 0) ctx->landing_stable_since = now;
        if (now - ctx->landing_stable_since >= 1000) {
            ctx->last_altitude = altitude;
            ctx->last_sample = now;
            return SEVT_LANDING;
        }
    } else {
        ctx->landing_stable_since = 0;
    }

    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return SEVT_NONE;
}

/* [FLT-LAND-06, FLT-RATE-04] */
static state_event_t detect_landed(flight_context_t *ctx, uint32_t now) {
    if (now - ctx->last_sample < 1000) return SEVT_NONE;

    int32_t altitude;
    read_and_filter_pressure(ctx, now, &altitude);
    buf_add(ctx, now - ctx->launch_time, ctx->filtered_pressure, altitude, LANDED);
    ctx->last_altitude = altitude;
    ctx->last_sample = now;
    return SEVT_NONE;
}

/* ── Transition actions ───────────────────────────────────────────── */

static void action_cal_init(flight_context_t *ctx, uint32_t now) {
    ctx->cal_count = 0;
    ctx->cal_sum = 0;
    ctx->boot_timer = now;
}

static void action_ground_cal(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->ground_pressure = ctx->cal_sum / 10;
}

/* [FLT-LAUNCH-03..05] Backdate launch time to first sample above 50cm */
static void action_launch(flight_context_t *ctx, uint32_t now) {
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
}

/* [FLT-ASC-05] */
static void action_armed(flight_context_t *ctx, uint32_t now) {
    (void)now;
    ctx->pyros_armed = true;
    buf_tag_event(ctx, EVT_ARMED);
}

/* [FLT-APO-03] */
static void action_apogee(flight_context_t *ctx, uint32_t now) {
    ctx->apogee_detected = true;
    ctx->apogee_time = now;
    buf_tag_event(ctx, EVT_APOGEE);
    try_fire_pyros(ctx, now);
}

/* [FLT-LAND-05, BUZ-03, DAT-06] */
static void action_landing(flight_context_t *ctx, uint32_t now) {
    (void)now;
    buf_tag_event(ctx, EVT_LANDING);
    buzzer_set_altitude(cm_to_units(ctx->max_altitude, ctx->config.units));
    ctx->landed_beep_started = true;
    ctx->csv_saved = true;
}

/* ── State machine ────────────────────────────────────────────────── */

static const detect_fn detectors[STATE_COUNT] = {
    [BOOT_INIT]       = detect_boot_init,
    [BOOT_SETTLE]     = detect_boot_settle,
    [BOOT_CONTINUITY] = detect_boot_continuity,
    [BOOT_CALIBRATE]  = detect_boot_calibrate,
    [PAD_IDLE]        = detect_pad_idle,
    [ASCENT]          = detect_ascent,
    [DESCENT]         = detect_descent,
    [LANDED]          = detect_landed,
};

static const transition_t transitions[] = {
    { BOOT_INIT,       SEVT_DONE,     BOOT_SETTLE,     NULL             },
    { BOOT_SETTLE,     SEVT_TIMER,    BOOT_CONTINUITY, NULL             },
    { BOOT_CONTINUITY, SEVT_DONE,     BOOT_CALIBRATE,  action_cal_init  },
    { BOOT_CALIBRATE,  SEVT_CAL_DONE, PAD_IDLE,        action_ground_cal},
    { PAD_IDLE,        SEVT_LAUNCH,   ASCENT,          action_launch    },
    { ASCENT,          SEVT_ARMED,    ASCENT,          action_armed     },
    { ASCENT,          SEVT_APOGEE,   DESCENT,         action_apogee    },
    { DESCENT,         SEVT_LANDING,  LANDED,          action_landing   },
};

#define NUM_TRANSITIONS (sizeof(transitions) / sizeof(transitions[0]))

flight_state_t dispatch_state(flight_context_t *ctx, uint32_t now) {
    if (ctx->current_state >= STATE_COUNT) return PAD_IDLE;

    detect_fn detect = detectors[ctx->current_state];
    if (!detect) return ctx->current_state;

    state_event_t evt = detect(ctx, now);
    if (evt == SEVT_NONE) return ctx->current_state;

    for (int i = 0; i < (int)NUM_TRANSITIONS; i++) {
        if (transitions[i].from == ctx->current_state && transitions[i].event == evt) {
            if (transitions[i].action) transitions[i].action(ctx, now);
            return transitions[i].to;
        }
    }
    return ctx->current_state;
}

/* ── CSV export ───────────────────────────────────────────────────── */

static const char *event_name(uint8_t evt) {
    switch (evt) {
        case EVT_LAUNCH: return "LAUNCH"; case EVT_ARMED: return "ARMED";
        case EVT_APOGEE: return "APOGEE"; case EVT_PYRO1_FIRE: return "PYRO1";
        case EVT_PYRO2_FIRE: return "PYRO2"; case EVT_LANDING: return "LANDING";
        default: return "";
    }
}

static const char *mode_name(uint8_t mode) {
    switch (mode) {
        case PYRO_MODE_DELAY: return "delay"; case PYRO_MODE_AGL: return "agl";
        case PYRO_MODE_FALLEN: return "fallen"; case PYRO_MODE_SPEED: return "speed";
        default: return "none";
    }
}

/* [DAT-06, DAT-07] */
int flight_save_csv(flight_context_t *ctx) {
    if (ctx->buf_count == 0) return -1;
    hal_file_t *f = hal_fs_open("flight.csv", false);
    if (!f) return -1;

    char line[128];
    int n = snprintf(line, sizeof(line),
        "# Pyro MK1B Flight Data\n# ID: %.8s\n# Name: %.8s\n"
        "# Pyro1: %s %u\n# Pyro2: %s %u\n"
        "# Units: %s\n# Ground Pa: %ld\n# Max Alt cm: %ld\n"
        "time_ms,pressure_pa,altitude_cm,state,thrust,event\n",
        ctx->config.id, ctx->config.name,
        mode_name(ctx->config.pyro1_mode), ctx->config.pyro1_value,
        mode_name(ctx->config.pyro2_mode), ctx->config.pyro2_value,
        ctx->config.units == 2 ? "ft" : ctx->config.units == 1 ? "m" : "cm",
        (long)ctx->ground_pressure, (long)ctx->max_altitude);
    hal_fs_write(f, line, n);

    uint16_t idx = ctx->buf_tail;
    for (int i = 0; i < ctx->buf_count; i++) {
        flight_sample_t *s = &ctx->flight_buffer[idx];
        n = snprintf(line, sizeof(line), "%lu,%ld,%ld,%u,%u,%s\n",
            (unsigned long)s->time_ms, (long)s->pressure_pa,
            (long)s->altitude_cm, s->state, s->under_thrust, event_name(s->event));
        hal_fs_write(f, line, n);
        idx = (idx + 1) % 4096;
    }
    hal_fs_close(f);
    return 0;
}

/* ── Init and output ──────────────────────────────────────────────── */

void flight_init(flight_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->config.id, "PYRO001", 8);
    strncpy(ctx->config.name, "PYRO001", 8);
    ctx->config.pyro1_mode = PYRO_MODE_DELAY;
    ctx->config.pyro1_value = 0;
    ctx->config.pyro2_mode = PYRO_MODE_AGL;
    ctx->config.pyro2_value = 300;
    ctx->config.units = 1;
    ctx->current_state = BOOT_INIT;
}

void flight_update_outputs(flight_context_t *ctx, uint32_t now) {
    hal_pyro_update(now);
    buzzer_update(now);

    if (ctx->current_state >= PAD_IDLE) {
        uint32_t interval = (ctx->current_state == ASCENT || ctx->current_state == DESCENT) ? 100 : 1000;
        if (now - ctx->last_telemetry >= interval) {
            uint32_t flight_time = (ctx->current_state != PAD_IDLE) ? (now - ctx->launch_time) : 0;
            send_telemetry(ctx, flight_time, ctx->last_altitude, ctx->current_state);
            ctx->last_telemetry = now;
        }
    }
}
