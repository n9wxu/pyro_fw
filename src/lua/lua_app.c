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
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include <stdio.h>
#include <string.h>

static char script_buf[LUA_SCRIPT_MAX];
static int script_len;
static char status_line[96] = "off";
static bool launch_pending;
static uint32_t launch_at_ms;
/* Distinguishes "core1 was started and has not reported RUNNING" from "core1
 * was never started" -- the safe-boot latch, no script, or Lua disabled. Both
 * leave the state at LUA_C1_OFF, and conflating them overwrites the message
 * that says which. */
static bool launched;

/* ── Safe boot ────────────────────────────────────────────────────
 *
 * A user program that takes the whole board down must not take it down
 * twice. Before core1 is launched a marker goes into a watchdog scratch
 * register; it is cleared once the system has proven it survives with core1
 * running. The registers survive a watchdog reset but not a power cycle or a
 * RUN reset, which is exactly the semantics wanted:
 *
 *   board wedges -> watchdog reboots -> marker still set -> Lua skipped,
 *   device comes up reachable and says why -> operator fixes the script.
 *
 * Power-cycling or pressing RUN clears it and lets the program try again, so
 * the latch never becomes something an operator has to remember to reset.
 *
 * This pairs with the boot watchdog in main_hardware.c. Without a watchdog
 * the board would simply hang and the marker would never be read, which is
 * how the first bench run needed the BOOTSEL button. */
#define LUA_BOOT_MARK 0x4C554131u /* "LUA1" */
#define LUA_BOOT_SCRATCH                                                                                               \
    3 /* 4..7 belong to the SDK: watchdog_enable() writes                                                              \
       * WATCHDOG_NON_REBOOT_MAGIC to scratch[4] and                                                                   \
       * watchdog_reboot() puts its vector in 4..7, so a                                                               \
       * marker there is overwritten or misread. 0..3 are                                                              \
       * unused by both the SDK and the bootloader. */
#define LUA_PHASE_SCRATCH 2
#define LUA_SETTLE_MS 15000u

/* Breadcrumb: where core0 was when it last stopped. Survives a watchdog
 * reboot, so the next boot can say what it was doing instead of leaving it to
 * be guessed at. */
#define PH_NO_LUA 1u
#define PH_PRELAUNCH 2u
#define PH_LAUNCHED 3u
#define PH_SETTLED 4u
#define PH_KILLED 5u

static uint32_t boot_phase;
static uint32_t last_stage;
static uint32_t last_stage_ms;

static void phase(uint32_t p) {
    watchdog_hw->scratch[LUA_PHASE_SCRATCH] = 0x50480000u | p;
}

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

static int script_read_err;

int lua_app_script_read(char *buf, int max) {
    int n = hal_fs_read_file(LUA_SCRIPT_PATH, buf, max - 1);
    script_read_err = n;
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

/* Build the resource set a configuration WOULD grant, without binding
 * anything. This is what the web check validates against, so an operator sees
 * a truthful verdict for the configuration they just saved rather than for
 * the one still running. */
static void env_from_config(const config_t *cfg, lua_chk_env_t *env) {
    memset(env, 0, sizeof(*env));
    const struct {
        const char *role;
        const char *name;
    } pins[4] = {
        {cfg->lua_p18_role, cfg->lua_p18_name},
        {cfg->lua_p19_role, cfg->lua_p19_name},
        {cfg->lua_p20_role, cfg->lua_p20_name},
        {cfg->lua_p21_role, cfg->lua_p21_name},
    };
    for (int i = 0; i < 4; i++) {
        lua_role_t r = role_of(pins[i].role);
        if (r == LUA_ROLE_OFF) {
            continue;
        }
        if (r == LUA_ROLE_PIXEL) {
            env->has_pixel = cfg->lua_pixels > 0;
            continue;
        }
        if (env->n < LUA_CHK_MAX_NAMES) {
            strncpy(env->names[env->n++], pins[i].name, LUA_NAME_MAX - 1);
        }
        switch (r) {
        case LUA_ROLE_OUT:
        case LUA_ROLE_PWM:
            env->has_output = true;
            break;
        case LUA_ROLE_IN:
            env->has_input = true;
            break;
        case LUA_ROLE_TX:
        case LUA_ROLE_RX:
            env->has_serial = true;
            break;
        default:
            break;
        }
    }
}

void lua_app_check(const char *src, int len, const config_t *cfg, lua_chk_result_t *out) {
    lua_chk_env_t env;
    if (cfg) {
        env_from_config(cfg, &env);
    } else {
        lua_chk_env_from_platform(&env);
    }
    lua_check(src, (size_t)len, &env, out);
}

int lua_app_console_read(char *buf, int max) {
    return lua_core1_console_read(buf, max);
}

/* ── Script log ───────────────────────────────────────────────────
 *
 * Core1 hands bytes over; core0 owns the file. A separate file rather than
 * the flight CSV because hal.h's event field is a uint8_t enum code, not a
 * string -- routing free-form text through it would mean widening a contract
 * every board implements, for one caller.
 *
 * Buffered and flushed on a watermark or a deadline, never per call. A
 * per-call write would put a flash erase wherever a script happened to call
 * log(), which is the same mistake hal_log_sample() makes today at
 * hal_common.c and the reason its flush needs moving. */
#define LUA_LOG_PATH "lua_log.txt"
#define LUA_LOG_BUF 512
#define LUA_LOG_FLUSH_MS 1000u

static char log_buf[LUA_LOG_BUF];
static int log_len;
static uint32_t log_due_ms;
static uint32_t log_written;

static void log_flush(void) {
    if (log_len <= 0) {
        return;
    }
    hal_file_t *f = hal_fs_open(LUA_LOG_PATH, true /* append */);
    if (f) {
        hal_fs_write(f, log_buf, log_len);
        hal_fs_close(f);
        log_written += (uint32_t)log_len;
    }
    /* Dropped either way. A filesystem that will not take the line is not
     * something to retry from the flight loop. */
    log_len = 0;
}

static void log_service(uint32_t now_ms) {
    int n = lua_core1_log_read(log_buf + log_len, LUA_LOG_BUF - log_len);
    log_len += n;

    if (log_len >= LUA_LOG_BUF - 64) {
        log_flush();
        log_due_ms = now_ms + LUA_LOG_FLUSH_MS;
        return;
    }
    if (log_len > 0 && (int32_t)(now_ms - log_due_ms) >= 0) {
        log_flush();
        log_due_ms = now_ms + LUA_LOG_FLUSH_MS;
    }
}

uint32_t lua_app_log_written(void) {
    return log_written;
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
        snprintf(status_line, sizeof(status_line), "enabled, no script (read %s = %d)", LUA_SCRIPT_PATH,
                 script_read_err);
        return;
    }

    /* Validate before launching. A script that cannot match the configuration
     * is not started at all, so its first failure is on the bench rather than
     * in the air. */
    lua_chk_result_t chk;
    lua_chk_env_t env;
    lua_chk_env_from_platform(&env); /* at boot the two agree, by definition */
    lua_check(script_buf, (size_t)script_len, &env, &chk);
    if (!chk.green) {
        snprintf(status_line, sizeof(status_line), "not started: %s", chk.items[0].detail);
        return;
    }

    uint32_t prev = watchdog_hw->scratch[LUA_PHASE_SCRATCH];
    boot_phase = ((prev & 0xffff0000u) == 0x50480000u) ? (prev & 0xffffu) : 0u;
    uint32_t st = watchdog_hw->scratch[0];
    last_stage = ((st & 0xffff0000u) == 0x53540000u) ? (st & 0xffffu) : 0u;
    last_stage_ms = watchdog_hw->scratch[1];

    if (watchdog_hw->scratch[LUA_BOOT_SCRATCH] == LUA_BOOT_MARK) {
        /* Last boot set this and never got far enough to clear it. */
        watchdog_hw->scratch[LUA_BOOT_SCRATCH] = 0;
        snprintf(status_line, sizeof(status_line), "disabled: last boot died in stage %lu at %lu ms (phase %lu)",
                 (unsigned long)last_stage, (unsigned long)last_stage_ms, (unsigned long)boot_phase);
        phase(PH_NO_LUA);
        return;
    }

    /* Deferred on purpose. Launching core1 inside init would put a user
     * program between power-on and the first HTTP response, so a program that
     * wedges the board would also block the only route in to replace it.
     * Starting it a couple of seconds into the main loop means the web
     * interface is already answering before any user code exists. */
    launch_pending = true;
    launch_at_ms = 0;
    phase(PH_PRELAUNCH);
    snprintf(status_line, sizeof(status_line), "starting");
}

/* ── Main loop ────────────────────────────────────────────────────── */

void lua_app_service(const flight_context_t *ctx, uint32_t now_ms) {
    if (launch_pending) {
        if (launch_at_ms == 0) {
            launch_at_ms = now_ms + 2000u;
            return;
        }
        if ((int32_t)(now_ms - launch_at_ms) < 0) {
            return;
        }
        launch_pending = false;
        watchdog_hw->scratch[LUA_BOOT_SCRATCH] = LUA_BOOT_MARK;
        phase(PH_LAUNCHED);
        launched = true;
        lua_core1_start(script_buf, script_len);
        phase(17);
        snprintf(status_line, sizeof(status_line), "running (%d out, %d in, %d serial, %d px)", lua_plat_output_count(),
                 lua_plat_input_count(), lua_plat_serial_count(), lua_plat_pixel_count());
        phase(18);
        return;
    }

    if (lua_core1_state() == LUA_C1_OFF) {
        /* Core1 was launched but has not reported RUNNING. It is wedged in
         * pyro_lua_init() or pyro_lua_load(), and the old early-return hid
         * every diagnostic for exactly that case. Say where it stopped. */
        if (launched && strncmp(status_line, "stuck", 5) != 0) {
            uint32_t loc = lua_core1_loc();
            snprintf(status_line, sizeof(status_line), "stuck before RUNNING [loc=%lu hb=%lu stackfree=%lu]",
                     (unsigned long)(loc & 0xff), (unsigned long)lua_core1_heartbeat(),
                     (unsigned long)lua_core1_stack_free());
        }
        return;
    }

    /* Clear the safe-boot marker once the board has demonstrably survived
     * with core1 running. A later crash is then a crash, not a bad boot. */
    if (now_ms > LUA_SETTLE_MS && watchdog_hw->scratch[LUA_BOOT_SCRATCH] == LUA_BOOT_MARK) {
        watchdog_hw->scratch[LUA_BOOT_SCRATCH] = 0;
        phase(PH_SETTLED);
    }

    lua_core1_check_stack();

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
    f.pyro_adc[0] = ctx->pyro1_adc;
    f.pyro_adc[1] = ctx->pyro2_adc;
    f.under_thrust = ctx->under_thrust ? 1 : 0;
    f.apogee_detected = ctx->apogee_detected ? 1 : 0;
    f.telem_seq = ctx->telemetry_seq;
    lua_core1_publish(&f);

    lua_core1_service(now_ms);
    log_service(now_ms);

    if (lua_core1_state() == LUA_C1_DEAD && strncmp(status_line, "stopped", 7) != 0) {
        phase(PH_KILLED);
        uint32_t ok, rq, ak, hb;
        lua_core1_park_stats(&ok, &rq, &ak, &hb);
        uint32_t loc = lua_core1_loc();
        snprintf(status_line, sizeof(status_line),
                 "stopped: %s [ok=%lu req=%lu ack=%lu hb=%lu loc=%lu failloc=%lu preq=%lu stack=%lu]",
                 lua_core1_error(), (unsigned long)ok, (unsigned long)rq, (unsigned long)ak, (unsigned long)hb,
                 (unsigned long)(loc & 0xff), (unsigned long)((loc >> 8) & 0xff), (unsigned long)((loc >> 16) & 0xff),
                 (unsigned long)lua_core1_stack_free());
    }
}

void lua_app_event(const char *name) {
    lua_core1_event(name);
}
