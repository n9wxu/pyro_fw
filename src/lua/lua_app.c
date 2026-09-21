/*
 * Application glue for Lua. See lua_app.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_app.h"
#include "hal.h"
#include "flight_states.h"
#include "lua_core1.h"
#include "lua_platform.h"
#include "lua_platform_cfg.h"
#include <stdio.h>
#include <string.h>

static char script_buf[LUA_SCRIPT_MAX];
static int script_len;
static char status_line[96] = "off";

/* ── Config -> platform ───────────────────────────────────────────── */

static lua_role_t role_of(const char *s) {
    if (strcmp(s, "out") == 0) {
        return LUA_ROLE_OUT;
    }
    if (strcmp(s, "pwm") == 0) {
        return LUA_ROLE_PWM;
    }
    if (strcmp(s, "in") == 0) {
        return LUA_ROLE_IN;
    }
    if (strcmp(s, "tx") == 0) {
        return LUA_ROLE_TX;
    }
    if (strcmp(s, "rx") == 0) {
        return LUA_ROLE_RX;
    }
    if (strcmp(s, "pixel") == 0) {
        return LUA_ROLE_PIXEL;
    }
    return LUA_ROLE_OFF;
}

int lua_app_script_read(char *buf, int max) {
    int n = hal_fs_read_file(LUA_SCRIPT_PATH, buf, max - 1);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';
    return n;
}

bool lua_app_script_write(const char *src, int len) {
    if (len < 0 || len > LUA_SCRIPT_MAX - 1) {
        return false;
    }
    return hal_fs_write_file(LUA_SCRIPT_PATH, src, len) == 0;
}

void lua_app_check(const char *src, int len, lua_chk_result_t *out) {
    lua_check(src, (size_t)len, out);
}

int lua_app_console_read(char *buf, int max) {
    return lua_core1_console_read(buf, max);
}

const char *lua_app_status(void) {
    return status_line;
}

void lua_app_init(const config_t *cfg) {
    if (!cfg->lua_enabled) {
        snprintf(status_line, sizeof(status_line), "disabled");
        return;
    }

    const lua_pin_cfg_t pins[4] = {
        {role_of(cfg->lua_p18_role), cfg->lua_p18_name},
        {role_of(cfg->lua_p19_role), cfg->lua_p19_name},
        {role_of(cfg->lua_p20_role), cfg->lua_p20_name},
        {role_of(cfg->lua_p21_role), cfg->lua_p21_name},
    };

    if (lua_plat_configure(pins, 4, cfg->lua_baud, cfg->lua_pixels) != 0) {
        /* Unreachable by the budget in the board platform file. If it ever
         * happens it is a build-time mistake, not an operating condition, so
         * say so plainly rather than degrading quietly. */
        snprintf(status_line, sizeof(status_line), "resource claim failed (firmware bug)");
        return;
    }

    script_len = lua_app_script_read(script_buf, sizeof(script_buf));
    if (script_len == 0) {
        snprintf(status_line, sizeof(status_line), "enabled, no script");
        return;
    }

    /* Validate before launching. A script that cannot match the configuration
     * is not started at all, so its first failure is on the bench rather than
     * in the air. */
    lua_chk_result_t chk;
    lua_check(script_buf, (size_t)script_len, &chk);
    if (!chk.green) {
        snprintf(status_line, sizeof(status_line), "not started: %s", chk.items[0].detail);
        return;
    }

    lua_core1_start(script_buf, script_len);
    snprintf(status_line, sizeof(status_line), "running (%d out, %d in, %d serial, %d px)", lua_plat_output_count(),
             lua_plat_input_count(), lua_plat_serial_count(), lua_plat_pixel_count());
}

/* ── Main loop ────────────────────────────────────────────────────── */

void lua_app_service(const flight_context_t *ctx, uint32_t now_ms) {
    if (lua_core1_state() == LUA_C1_OFF) {
        return;
    }

    /* Publish flight state for the script to read. The seqlock write never
     * waits, so this costs core0 a fixed handful of stores whatever core1 is
     * doing -- including nothing at all. */
    lua_flight_t f;
    f.pressure_pa = ctx->filtered_pressure;
    f.altitude_cm = ctx->last_altitude;
    f.speed_cms = ctx->vertical_speed_cms;
    f.max_alt_cm = ctx->max_altitude;
    f.state = (int)ctx->current_state;
    f.time_ms = now_ms;
    f.pyro[0] = (ctx->pyro1_continuity_good ? LUA_PYRO_CONTINUITY : 0) | (ctx->pyro1_fired ? LUA_PYRO_FIRED : 0) |
                (ctx->pyro1_fault ? LUA_PYRO_FAULT : 0) | (ctx->pyros_armed ? LUA_PYRO_ARMED : 0);
    f.pyro[1] = (ctx->pyro2_continuity_good ? LUA_PYRO_CONTINUITY : 0) | (ctx->pyro2_fired ? LUA_PYRO_FIRED : 0) |
                (ctx->pyro2_fault ? LUA_PYRO_FAULT : 0) | (ctx->pyros_armed ? LUA_PYRO_ARMED : 0);
    lua_core1_publish(&f);

    lua_core1_service(now_ms);

    if (lua_core1_state() == LUA_C1_DEAD && strncmp(status_line, "stopped", 7) != 0) {
        snprintf(status_line, sizeof(status_line), "stopped: %s", lua_core1_error());
    }
}

void lua_app_event(const char *name) {
    lua_core1_event(name);
}
