/*
 * /api/status rendered from a snapshot.
 *
 * The expected text is composed here, key by key, independently of the
 * renderer: the keys, their order and their formatting are the API the web
 * UI, support/api_check.py and every bench script read. The build links
 * status_json.c alone, so a renderer that reached for live state would not
 * link.
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "../src/status_json.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static char buf[8192];

void setUp(void) {
    memset(buf, 0x55, sizeof(buf));
}

void tearDown(void) {
}

static void typical(status_snap_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = "PAD_IDLE";
    s->alt_cm = 1234;
    s->max_alt_cm = 5678;
    s->vspeed_cms = -12;
    s->pressure_pa = 101325;
    s->pyro_cont[0] = true;
    s->pyro_adc[0] = 17;
    s->pyro_adc[1] = 4095;
    s->pyro_fired[1] = true;
    s->armed = true;
    s->flight_ms = 0;
    s->uptime_ms = 123456;
    s->fw_version = "2.1.999";
    s->pyro_mode[0] = "fallen";
    s->pyro_mode[1] = "agl";
    s->pyro_value[1] = 150;
    s->units = 1;
    strcpy(s->rocket_id, "R1");
    strcpy(s->rocket_name, "Test");
    s->sensor = "MS5607";
    s->board = "Pyro MK1C";
    s->pyro_bus_q = -1;
    s->pyro_bus_adc = 812;
    s->pyro_vbat_adc = 2900;
    s->loop_max_us = 4000;
    s->loop_overruns = 2;
    s->loop_late_max_us = 150;
    s->loop_count = 99999;
    for (int i = 0; i < STATUS_STAGES; i++) {
        s->stage_max_us[i] = (uint32_t)(i + 1);
    }
    for (int i = 0; i < STATUS_STAGE1_PARTS; i++) {
        s->stage1_parts_us[i] = (uint32_t)(10 * (i + 1));
    }
    s->http_units[0] = 5;
    s->http_units[1] = 6;
    s->http_unit_max_us[0] = 700;
    s->http_unit_max_us[1] = 800;
    s->flash_opens = 11;
    s->flash_skips = 12;
    s->flash_erases = 3;
    s->flash_programs = 4;
    s->flash_deferrals = 5;
    strcpy(s->pins_reason, "ok");
    s->pyro_released[1] = true;
    s->bridge = true;
    s->bridge_ch = 2;
    s->bridge_common = 3;
    s->pyro_real[0] = true;
    s->sensor_ok = true;
    s->fs_ok = true;
    s->faults[0] = "p2_open";
    s->faults[1] = "cfg_range";
    s->n_faults = 2;
    s->reset_cause = 1;
    s->recovery = "cold boot";
    s->prev_watchdog = true;
    s->prev_stage = 96;
    s->prev_stage_ms = 204511;
    s->pyro_refused[1] = true;
    s->pyro1_refires = 1;
    s->pres_waits = 7;
    s->raw_pa = 101300;
    s->pad_speed_cms = -3;
    s->ground_reseeds = 2;
    s->sample_interval_us[0] = 9900;
    s->sample_interval_us[1] = 10100;
    s->stamp_lag_max_us = 300;
    s->fit_sigma_mpa = 2400;
    s->usb_attached = true;
    s->beep = "ready";
    s->beep_kind = "code";
    s->beep_is_code = true;
    s->beep_d1 = 3;
    s->beep_d2 = 2;
    strcpy(s->serial, "0202840A6B01");
    s->serial_assigned = true;
    strcpy(s->hw_id, "E6614104031F5A2B");
    s->subnet = 11;
}

/* Today's /api/status, key for key and in order, for typical(). */
static const char *const TYPICAL[][2] = {
    {"state", "\"PAD_IDLE\""},
    {"alt_cm", "1234"},
    {"max_alt_cm", "5678"},
    {"vspeed_cms", "-12"},
    {"pressure_pa", "101325"},
    {"pyro1_cont", "true"},
    {"pyro2_cont", "false"},
    {"pyro1_adc", "17"},
    {"pyro2_adc", "4095"},
    {"pyro1_fired", "false"},
    {"pyro2_fired", "true"},
    {"armed", "true"},
    {"flight_ms", "0"},
    {"uptime", "123456"},
    {"fw_version", "\"2.1.999\""},
    {"pyro1_mode", "\"fallen\""},
    {"pyro1_value", "0"},
    {"pyro2_mode", "\"agl\""},
    {"pyro2_value", "150"},
    {"units", "1"},
    {"rocket_id", "\"R1\""},
    {"rocket_name", "\"Test\""},
    {"sensor", "\"MS5607\""},
    {"board", "\"Pyro MK1C\""},
    {"pyro_bus_q", "-1"},
    {"pyro_bus_adc", "812"},
    {"pyro_vbat_adc", "2900"},
    {"loop_max_us", "4000"},
    {"loop_overruns", "2"},
    {"loop_late_max_us", "150"},
    {"loop_count", "99999"},
    {"stage_max_us", "[1,2,3,4,5,6,7,8,9]"},
    {"stage1_parts_us", "[10,20,30,40]"},
    {"http_units", "[5,6]"},
    {"http_unit_max_us", "[700,800]"},
    {"flash_opens", "11"},
    {"flash_skips", "12"},
    {"flash_refusals", "0"},
    {"log_dropped", "0"},
    {"flash_erases", "3"},
    {"flash_programs", "4"},
    {"flash_deferrals", "5"},
    {"pins_reason", "\"ok\""},
    {"pyro1_released", "false"},
    {"pyro2_released", "true"},
    {"bridge", "\"2+3\""},
    {"pyro_mocked", "0"},
    {"pyro1_real", "true"},
    {"pyro2_real", "false"},
    {"sensor_ok", "true"},
    {"fs_ok", "true"},
    {"faults", "[\"p2_open\",\"cfg_range\"]"},
    {"reset_cause", "1"},
    {"recovery", "\"cold boot\""},
    {"prev_watchdog", "true"},
    {"prev_stage", "96"},
    {"prev_stage_ms", "204511"},
    {"pyro1_refused", "false"},
    {"pyro2_refused", "true"},
    {"pyro1_refires", "1"},
    {"main_forced", "false"},
    {"pres_waits", "7"},
    {"pres_rejects", "0"},
    {"raw_pa", "101300"},
    {"pad_speed_cms", "-3"},
    {"ground_degraded", "false"},
    {"ground_reseeds", "2"},
    {"sample_interval_us", "[9900,10100]"},
    {"stamp_lag_max_us", "300"},
    {"fit_sigma_mpa", "2400"},
    {"mach_lock", "false"},
    {"mach_flag_ms", "0"},
    {"peak_lower_bound", "false"},
    {"usb_attached", "true"},
    {"test_mode", "false"},
    {"buzzer_active", "false"},
    {"beep", "\"ready\""},
    {"beep_sound", "\"3-2\""},
    {"serial", "\"0202840A6B01\""},
    {"serial_assigned", "true"},
    {"hw_id", "\"E6614104031F5A2B\""},
    {"subnet", "11"},
};

static void compose(char *out, size_t cap, const char *const kv[][2], size_t n) {
    size_t pos = (size_t)snprintf(out, cap, "{");
    for (size_t i = 0; i < n; i++) {
        pos += (size_t)snprintf(out + pos, cap - pos, "%s\"%s\":%s", i ? "," : "", kv[i][0], kv[i][1]);
    }
    snprintf(out + pos, cap - pos, "}");
}

/* ── A JSON reader, strict enough to reject what a browser rejects ─── */

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    return p;
}

static const char *value(const char *p);

static const char *string(const char *p) {
    if (*p != '"') {
        return NULL;
    }
    for (p++; *p != '"'; p++) {
        if ((unsigned char)*p < 0x20) {
            return NULL;
        }
        if (*p == '\\') {
            p++;
            if (!strchr("\"\\/bfnrtu", *p)) {
                return NULL;
            }
        }
    }
    return p + 1;
}

static const char *number(const char *p) {
    const char *s = p;
    if (*p == '-') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    return p > s && p[-1] != '-' ? p : NULL;
}

static const char *container(const char *p, char close, bool keyed) {
    p = ws(p + 1);
    if (*p == close) {
        return p + 1;
    }
    for (;;) {
        if (keyed) {
            p = string(ws(p));
            if (!p || *(p = ws(p)) != ':') {
                return NULL;
            }
            p++;
        }
        p = value(ws(p));
        if (!p) {
            return NULL;
        }
        p = ws(p);
        if (*p == close) {
            return p + 1;
        }
        if (*p != ',') {
            return NULL;
        }
        p++;
    }
}

static const char *value(const char *p) {
    if (*p == '{') {
        return container(p, '}', true);
    }
    if (*p == '[') {
        return container(p, ']', false);
    }
    if (*p == '"') {
        return string(p);
    }
    if (strncmp(p, "true", 4) == 0) {
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        return p + 5;
    }
    return number(p);
}

static void assert_json(const char *text) {
    const char *end = value(ws(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "not JSON");
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', *ws(end), "text after the JSON value");
}

/* ── Tests ────────────────────────────────────────────────────────── */

void test_SJ_01_keys_order_and_formatting_are_the_api(void) {
    status_snap_t s;
    typical(&s);
    static char want[8192];
    compose(want, sizeof(want), TYPICAL, sizeof(TYPICAL) / sizeof(TYPICAL[0]));
    int n = status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    assert_json(buf);
}

void test_SJ_02_the_widest_status_fits_its_bound(void) {
    static const char *const long40 = "0123456789012345678901234567890123456789";
    status_snap_t s;
    typical(&s);
    s.state = s.fw_version = s.sensor = s.board = s.recovery = s.beep = s.beep_kind = long40;
    s.pyro_mode[0] = s.pyro_mode[1] = long40;
    s.alt_cm = s.max_alt_cm = s.vspeed_cms = s.pressure_pa = s.prev_stage = INT32_MIN;
    s.raw_pa = s.pad_speed_cms = s.pyro_bus_q = s.pyro_bus_adc = s.pyro_vbat_adc = INT32_MIN;
    uint32_t *u32[] = {&s.flight_ms,        &s.uptime_ms,        &s.loop_max_us,        &s.loop_overruns,
                       &s.loop_late_max_us, &s.loop_count,       &s.flash_opens,        &s.flash_skips,
                       &s.flash_refusals,   &s.log_dropped,      &s.flash_erases,       &s.flash_programs,
                       &s.flash_deferrals,  &s.pyro_mocked,      &s.pres_waits,         &s.pres_rejects,
                       &s.ground_reseeds,   &s.stamp_lag_max_us, &s.fit_sigma_mpa,      &s.mach_flag_ms,
                       &s.http_units[0],    &s.http_units[1],    &s.http_unit_max_us[0], &s.http_unit_max_us[1],
                       &s.sample_interval_us[0], &s.sample_interval_us[1], &s.prev_stage_ms};
    for (size_t i = 0; i < sizeof(u32) / sizeof(u32[0]); i++) {
        *u32[i] = UINT32_MAX;
    }
    for (int i = 0; i < STATUS_STAGES; i++) {
        s.stage_max_us[i] = UINT32_MAX;
    }
    for (int i = 0; i < STATUS_STAGE1_PARTS; i++) {
        s.stage1_parts_us[i] = UINT32_MAX;
    }
    s.pyro_adc[0] = s.pyro_adc[1] = s.pyro_value[0] = s.pyro_value[1] = UINT16_MAX;
    s.units = s.reset_cause = s.pyro1_refires = s.subnet = s.bridge_ch = s.bridge_common = UINT8_MAX;
    s.beep_d1 = s.beep_d2 = UINT8_MAX;
    memset(s.rocket_id, '"', 8);
    memset(s.rocket_name, '\\', 8);
    memset(s.pins_reason, '"', sizeof(s.pins_reason) - 1);
    s.pins_reason[sizeof(s.pins_reason) - 1] = '\0';
    memset(s.serial, 'F', sizeof(s.serial) - 1);
    memset(s.hw_id, 'F', sizeof(s.hw_id) - 1);
    for (int i = 0; i < STATUS_FAULTS_MAX; i++) {
        s.faults[i] = "a_fault_named_at_length_24";
    }
    s.n_faults = STATUS_FAULTS_MAX;

    int n = status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_LESS_THAN_INT(STATUS_JSON_MAX, n);
    assert_json(buf);
}

/* An operator names the rocket; a name with a quote in it must not end the
 * JSON string. */
void test_SJ_03_strings_are_escaped(void) {
    status_snap_t s;
    typical(&s);
    strcpy(s.rocket_id, "a\"b");
    strcpy(s.rocket_name, "c\\d");
    strcpy(s.pins_reason, "pin \"8\"\nrejected");
    int n = status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    assert_json(buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rocket_id\":\"a\\\"b\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rocket_name\":\"c\\\\d\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pins_reason\":\"pin \\\"8\\\"\\nrejected\""));
}

void test_SJ_04_a_buffer_too_small_is_refused_not_truncated(void) {
    status_snap_t s;
    typical(&s);
    int n = status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(n, status_json(&s, buf, (size_t)n + 1));
    TEST_ASSERT_EQUAL_INT(-1, status_json(&s, buf, (size_t)n));
    TEST_ASSERT_EQUAL_INT(-1, status_json(&s, buf, 64));
}

/* A boot the watchdog did not end, or one with no stage stamped, says so. */
void test_SJ_07_no_previous_stage_reads_minus_one(void) {
    status_snap_t s;
    typical(&s);
    s.prev_watchdog = false;
    s.prev_stage = -1;
    s.prev_stage_ms = 0;
    status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"prev_watchdog\":false,\"prev_stage\":-1,\"prev_stage_ms\":0,"));
}

void test_SJ_05_empty_and_alternative_forms(void) {
    status_snap_t s;
    typical(&s);
    s.n_faults = 0;
    s.bridge = false;
    s.beep_is_code = true;
    s.beep_d2 = 0;
    status_json(&s, buf, sizeof(buf));
    assert_json(buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"faults\":[],"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"bridge\":\"none\","));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"beep_sound\":\"3\","));

    s.beep_is_code = false;
    s.beep_kind = "chirp";
    status_json(&s, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"beep_sound\":\"chirp\","));
}

void test_SJ_06_escaping_truncates_rather_than_overflows(void) {
    char out[16];
    memset(out, 0x55, sizeof(out));
    json_escape(out, (int)sizeof(out), "\"\"\"\"\"\"\"\"\"\"\"\"", 12);
    TEST_ASSERT_LESS_THAN_size_t(sizeof(out), strlen(out));
    json_escape(out, (int)sizeof(out), "a\x01\x1f" "b\n", 5);
    TEST_ASSERT_EQUAL_STRING("ab\\n", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_SJ_01_keys_order_and_formatting_are_the_api);
    RUN_TEST(test_SJ_02_the_widest_status_fits_its_bound);
    RUN_TEST(test_SJ_03_strings_are_escaped);
    RUN_TEST(test_SJ_04_a_buffer_too_small_is_refused_not_truncated);
    RUN_TEST(test_SJ_05_empty_and_alternative_forms);
    RUN_TEST(test_SJ_06_escaping_truncates_rather_than_overflows);
    RUN_TEST(test_SJ_07_no_previous_stage_reads_minus_one);
    return UNITY_END();
}
