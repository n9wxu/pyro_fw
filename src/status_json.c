/*
 * SPDX-License-Identifier: MIT
 */
#include "status_json.h"
#include <stdarg.h>
#include <stdio.h>

void json_escape(char *out, int out_sz, const char *in, int len) {
    int j = 0;
    for (int i = 0; i < len && j < out_sz - 8; i++) {
        char ch = in[i];
        if (ch == '"' || ch == '\\') {
            out[j++] = '\\';
            out[j++] = ch;
        } else if (ch == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if ((unsigned char)ch >= 0x20) {
            out[j++] = ch;
        }
    }
    out[j] = '\0';
}

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    bool over;
} out_t;

static void put(out_t *o, const char *fmt, ...) {
    if (o->over) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->pos, o->cap - o->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->pos) {
        o->over = true;
        return;
    }
    o->pos += (size_t)n;
}

static const char *B(bool b) {
    return b ? "true" : "false";
}

static const char *selftest_name(uint8_t v) {
    switch (v) {
    case 1:
        return "pass";
    case 2:
        return "fail";
    default:
        return "unknown";
    }
}

static const char *S(const char *s) {
    return s ? s : "";
}

static void escape_field(char *out, int out_sz, const char *in, int max) {
    int len = 0;
    while (len < max && in[len]) {
        len++;
    }
    json_escape(out, out_sz, in, len);
}

int status_json(const status_snap_t *s, char *buf, size_t cap) {
    char id[2 * sizeof(s->rocket_id) + 8], name[2 * sizeof(s->rocket_name) + 8];
    char reason[2 * sizeof(s->pins_reason) + 8];
    escape_field(id, (int)sizeof(id), s->rocket_id, (int)sizeof(s->rocket_id) - 1);
    escape_field(name, (int)sizeof(name), s->rocket_name, (int)sizeof(s->rocket_name) - 1);
    escape_field(reason, (int)sizeof(reason), s->pins_reason, (int)sizeof(s->pins_reason) - 1);

    char bridge[12] = "none";
    if (s->bridge) {
        snprintf(bridge, sizeof(bridge), "%u+%u", (unsigned)s->bridge_ch, (unsigned)s->bridge_common);
    }
    char sound[12];
    if (!s->beep_is_code) {
        snprintf(sound, sizeof(sound), "%s", S(s->beep_kind));
    } else if (s->beep_d2 == 0) {
        snprintf(sound, sizeof(sound), "%u", (unsigned)s->beep_d1);
    } else {
        snprintf(sound, sizeof(sound), "%u-%u", (unsigned)s->beep_d1, (unsigned)s->beep_d2);
    }

    const uint32_t *st = s->stage_max_us;
    const uint32_t *p1 = s->stage1_parts_us;
    out_t o = {buf, cap, 0, cap == 0};
    put(&o,
        "{\"state\":\"%s\",\"alt_cm\":%ld,\"max_alt_cm\":%ld,\"vspeed_cms\":%ld,\"pressure_pa\":%ld,"
        "\"pyro1_cont\":%s,\"pyro2_cont\":%s,\"pyro1_adc\":%u,\"pyro2_adc\":%u,"
        "\"pyro1_fired\":%s,\"pyro2_fired\":%s,"
        "\"armed\":%s,\"flight_ms\":%lu,\"uptime\":%lu,\"fw_version\":\"%s\","
        "\"pyro1_mode\":\"%s\",\"pyro1_value\":%u,\"pyro2_mode\":\"%s\",\"pyro2_value\":%u,"
        "\"units\":%u,\"log_rate\":\"%s\",\"rocket_id\":\"%s\",\"rocket_name\":\"%s\",\"sensor\":\"%s\","
        "\"board\":\"%s\",\"board_id\":\"%s\",\"board_selftest\":\"%s\","
        "\"pyro_bus_q\":%ld,\"pyro_bus_adc\":%ld,\"pyro_vbat_adc\":%ld,",
        S(s->state), (long)s->alt_cm, (long)s->max_alt_cm, (long)s->vspeed_cms, (long)s->pressure_pa,
        B(s->pyro_cont[0]), B(s->pyro_cont[1]), (unsigned)s->pyro_adc[0], (unsigned)s->pyro_adc[1], B(s->pyro_fired[0]),
        B(s->pyro_fired[1]), B(s->armed), (unsigned long)s->flight_ms, (unsigned long)s->uptime_ms, S(s->fw_version),
        S(s->pyro_mode[0]), (unsigned)s->pyro_value[0], S(s->pyro_mode[1]), (unsigned)s->pyro_value[1],
        (unsigned)s->units, S(s->log_rate), id, name, S(s->sensor), S(s->board), S(s->board_id),
        selftest_name(s->board_selftest), (long)s->pyro_bus_q, (long)s->pyro_bus_adc, (long)s->pyro_vbat_adc);
    put(&o,
        "\"loop_max_us\":%lu,\"loop_overruns\":%lu,\"loop_late_max_us\":%lu,\"loop_count\":%lu,"
        "\"stage_max_us\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"stage1_parts_us\":[%lu,%lu,%lu,%lu],"
        "\"http_units\":[%lu,%lu],\"http_unit_max_us\":[%lu,%lu],",
        (unsigned long)s->loop_max_us, (unsigned long)s->loop_overruns, (unsigned long)s->loop_late_max_us,
        (unsigned long)s->loop_count, (unsigned long)st[0], (unsigned long)st[1], (unsigned long)st[2],
        (unsigned long)st[3], (unsigned long)st[4], (unsigned long)st[5], (unsigned long)st[6], (unsigned long)st[7],
        (unsigned long)st[8], (unsigned long)p1[0], (unsigned long)p1[1], (unsigned long)p1[2], (unsigned long)p1[3],
        (unsigned long)s->http_units[0], (unsigned long)s->http_units[1], (unsigned long)s->http_unit_max_us[0],
        (unsigned long)s->http_unit_max_us[1]);
    put(&o,
        "\"flash_opens\":%lu,\"flash_skips\":%lu,\"flash_refusals\":%lu,\"log_dropped\":%lu,"
        "\"flash_erases\":%lu,\"flash_programs\":%lu,\"flash_deferrals\":%lu,"
        "\"pins_reason\":\"%s\",\"pyro1_released\":%s,\"pyro2_released\":%s,\"bridge\":\"%s\","
        "\"pyro_mocked\":%lu,\"pyro1_real\":%s,\"pyro2_real\":%s,\"sensor_ok\":%s,\"fs_ok\":%s,\"faults\":[",
        (unsigned long)s->flash_opens, (unsigned long)s->flash_skips, (unsigned long)s->flash_refusals,
        (unsigned long)s->log_dropped, (unsigned long)s->flash_erases, (unsigned long)s->flash_programs,
        (unsigned long)s->flash_deferrals, reason, B(s->pyro_released[0]), B(s->pyro_released[1]), bridge,
        (unsigned long)s->pyro_mocked, B(s->pyro_real[0]), B(s->pyro_real[1]), B(s->sensor_ok), B(s->fs_ok));
    for (int i = 0; i < s->n_faults && i < STATUS_FAULTS_MAX; i++) {
        put(&o, "%s\"%s\"", i ? "," : "", S(s->faults[i]));
    }
    put(&o,
        "],\"reset_cause\":%u,\"recovery\":\"%s\","
        "\"prev_watchdog\":%s,\"prev_stage\":%ld,\"prev_stage_ms\":%lu,"
        "\"pyro1_refused\":%s,\"pyro2_refused\":%s,\"pyro1_refires\":%u,\"main_forced\":%s,"
        "\"pres_waits\":%lu,\"pres_rejects\":%lu,\"pres_flashed\":%lu,\"raw_pa\":%ld,\"pad_speed_cms\":%ld,"
        "\"ground_degraded\":%s,\"ground_reseeds\":%lu,"
        "\"sample_interval_us\":[%lu,%lu],\"stamp_lag_max_us\":%lu,\"fit_sigma_mpa\":%lu,"
        "\"mach_lock\":%s,\"mach_flag_ms\":%lu,\"peak_lower_bound\":%s,"
        "\"usb_attached\":%s,\"test_mode\":%s,\"buzzer_active\":%s,\"beep\":\"%s\",\"beep_sound\":\"%s\","
        "\"serial\":\"%s\",\"serial_assigned\":%s,\"hw_id\":\"%s\",\"subnet\":%u,\"mac_source\":\"%s\"}",
        (unsigned)s->reset_cause, S(s->recovery), B(s->prev_watchdog), (long)s->prev_stage,
        (unsigned long)s->prev_stage_ms, B(s->pyro_refused[0]), B(s->pyro_refused[1]), (unsigned)s->pyro1_refires,
        B(s->main_forced), (unsigned long)s->pres_waits, (unsigned long)s->pres_rejects, (unsigned long)s->pres_flashed,
        (long)s->raw_pa, (long)s->pad_speed_cms, B(s->ground_degraded), (unsigned long)s->ground_reseeds,
        (unsigned long)s->sample_interval_us[0], (unsigned long)s->sample_interval_us[1],
        (unsigned long)s->stamp_lag_max_us, (unsigned long)s->fit_sigma_mpa, B(s->mach_lock),
        (unsigned long)s->mach_flag_ms, B(s->peak_lower_bound), B(s->usb_attached), B(s->test_mode),
        B(s->buzzer_active), S(s->beep), sound, s->serial, B(s->serial_assigned), s->hw_id, (unsigned)s->subnet,
        S(s->mac_source));
    return o.over ? -1 : (int)o.pos;
}
