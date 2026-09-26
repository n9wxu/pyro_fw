/*
 * Application glue for Lua. See lua_app.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_app.h"
#include "hal.h"
#include "flash_window.h"
#include "flight_states.h"
#include "lua_core1.h"
#include "lua_platform.h"
#include "lua_platform_cfg.h"
#include "pin_caps.h"
#include "pin_store.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include <stdio.h>
#include <string.h>

static char script_buf[LUA_SCRIPT_MAX];
static int script_len;
static char status_line[96] = "off";
static bool launch_pending;
static uint32_t launch_at_ms;
/* Both "started, not yet RUNNING" and "never started" leave the state at
 * LUA_C1_OFF, and conflating them overwrites the message saying which. */
static bool launched;

/* ── Safe boot ────────────────────────────────────────────────────
 *
 * A user program that takes the board down must not take it down twice:
 *
 *   board wedges -> watchdog reboots -> marker still set -> Lua skipped,
 *   device comes up reachable and says why -> operator fixes the script.
 *
 * Scratch registers survive a watchdog reset but not a power cycle or a RUN
 * reset, so either one clears the latch and no operator has to remember to.
 * Needs the boot watchdog in main_hardware.c, or the board hangs and nothing
 * reads the marker. */
#define LUA_BOOT_MARK 0x4C554131u /* "LUA1" */
#define LUA_BOOT_SCRATCH                                                                                               \
    3 /* 4..7 belong to the SDK: watchdog_enable() writes                                                              \
       * WATCHDOG_NON_REBOOT_MAGIC to scratch[4] and                                                                   \
       * watchdog_reboot() puts its vector in 4..7, so a                                                               \
       * marker there is overwritten or misread. 0..3 are                                                              \
       * unused by both the SDK and the bootloader. */
#define LUA_PHASE_SCRATCH 2
#define LUA_SETTLE_MS 15000u

/* Never a constant: a fixed grant handed out near the end of a 10 ms period
 * runs into the next period, where core0 wants its window. The grant is
 * what remains after the reserve, capped so a period whose work stages ran
 * quickly does not hand out a unit long enough to overrun anyway. */
#define LUA_FLASH_RESERVE_US 3000u
#define LUA_GRANT_MAX_US 5000u
#define LUA_GRANT_MIN_US 500u

/* Covers compiling the script and running init(), where a script may do real
 * work. Not unlimited: core0 writes no flash for the whole of startup, so a
 * core1 that never finishes costs logging and uploads for the flight. */
#define LUA_BOOT_LIMIT_MS 5000u

/* How long both sides of a half-bridge are held off between transitions, in
 * PIO cycles at 125 MHz -- 250 cycles is 2 us.
 *
 * A FET turns off in finite time, so this gap is what keeps a momentarily
 * conducting pair from becoming a shoot-through across VBAT. Generous for the
 * parts these boards fit, and tunable because the right value depends on
 * them: phase 5 measures it. */
#define PYRO_BRIDGE_DEADTIME_CYCLES 250u

/* Survives a watchdog reboot, so the next boot can report what core0 was
 * doing. */
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

/* The environment a script will actually get, built from the live pin
 * assignment rather than from config.ini.
 *
 * This used to read the lua_p18..p21 config keys. Those are gone -- pins.ini
 * superseded them -- and reading them was already wrong for two reasons: they
 * were MK1C-shaped four-entry positional slots, so on MK1B they described
 * pads that do not exist, and they could not describe a released pyro pad at
 * all.
 *
 * Binds nothing, so the check reports a verdict for the assignment as stored
 * rather than for whatever the running VM happens to hold. */
static void env_from_assignment(const config_t *cfg, lua_chk_env_t *env) {
    memset(env, 0, sizeof(*env));

    lua_pin_cfg_t pins[LUA_CFG_MAX];
    int n = pin_store_lua_pins(pins, LUA_CFG_MAX);

    for (int i = 0; i < n; i++) {
        switch (pins[i].role) {
        case LUA_ROLE_OFF:
            continue;
        case LUA_ROLE_PIXEL:
            env->has_pixel = cfg && cfg->lua_pixels > 0;
            continue;
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
        case LUA_ROLE_BRIDGE:
            /* A bridge is one output to a script; the pair is named once. */
            env->has_output = true;
            break;
        default:
            break;
        }
        if (pins[i].name && pins[i].name[0] && env->n < LUA_CHK_MAX_NAMES) {
            strncpy(env->names[env->n++], pins[i].name, LUA_NAME_MAX - 1);
        }
    }

    /* The bridge is not in the pad list -- it consumes two released pyro pads
     * and is configured separately. Its name still has to resolve. */
    uint8_t br_ch, br_common;
    const char *br_name;
    if (pin_store_bridge(&br_ch, &br_common, &br_name) && br_name && br_name[0]) {
        env->has_output = true;
        if (env->n < LUA_CHK_MAX_NAMES) {
            strncpy(env->names[env->n++], br_name, LUA_NAME_MAX - 1);
        }
    }
}

void lua_app_check(const char *src, int len, const config_t *cfg, lua_chk_result_t *out) {
    lua_chk_env_t env;
    if (cfg) {
        env_from_assignment(cfg, &env);
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
 * A script's log() output goes into the flight log as event rows, so it is
 * written through the window the samples already need.
 *
 * Do not open a file here. A second file contends with the flight log for
 * hal_fs_open()'s single streaming handle, which the flight log holds for
 * the whole of a flight.
 *
 * Logging therefore happens only during a flight. On the ground a script's
 * output goes to /api/lua/console, which needs no flash. */

/* Sized to fit inside hal_log_text()'s 80-byte row, of which the widest
 * timestamp plus the ",,,,,LUA " prefix take 19 and the newline one. Anything
 * over 59 is truncated there, and only once uptime reaches ten digits of
 * milliseconds -- a bug that appears after 27 hours and not before. */
#define LUA_LINE_MAX 56

static char line_buf[LUA_LINE_MAX];
static int line_len;
static uint32_t log_written;

static void line_emit(uint32_t now_ms) {
    if (line_len > 0 && hal_log_text(now_ms, line_buf, line_len)) {
        log_written += (uint32_t)line_len;
    }
    line_len = 0;
}

/* A carriage return ends a line as a newline does, so a script written for a
 * serial terminal does not produce one row per flight. */
static void log_drain(uint32_t now_ms) {
    char buf[128];
    int n;
    while ((n = lua_core1_log_read(buf, (int)sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                line_emit(now_ms);
            } else {
                line_buf[line_len++] = c;
                if (line_len == LUA_LINE_MAX) {
                    line_emit(now_ms);
                }
            }
        }
        if (n < (int)sizeof(buf)) {
            break;
        }
    }
}

/* Bytes of script output the flight log actually took. */
uint32_t lua_app_log_written(void) {
    return log_written;
}

const char *lua_app_status(void) {
    return status_line;
}

/* Separate from lua_app_ready() so flight_states.c needs no Lua header and a
 * board built without Lua resolves the weak default instead. */
bool lua_app_ready_or_absent(void) {
    return lua_app_ready();
}

bool lua_app_ready(void) {
    lua_c1_state_t st = lua_core1_state();
    if (st == LUA_C1_OFF || st == LUA_C1_DEAD) {
        return true; /* nothing to wait for */
    }
    return lua_core1_ready();
}

void lua_app_init(const config_t *cfg) {
    if (!cfg->lua_enabled) {
        snprintf(status_line, sizeof(status_line), "disabled");
        return;
    }

    /* From the live pin assignment rather than the lua_p* config keys. With
     * no pins.ini those keys are what the assignment was migrated FROM, so a
     * board that has never seen this feature keeps the pins it had. */
    lua_pin_cfg_t pins[LUA_CFG_MAX];
    int n_pins = pin_store_lua_pins(pins, LUA_CFG_MAX);

    /* The bridge is wired after the board's own pads, because its two pins
     * are not in LUA_PIN_LIST -- they only became available when
     * configuration released the channel. */
    uint8_t br_ch = 0, br_common = 0;
    const char *br_name = NULL;
    bool want_bridge = pin_store_bridge(&br_ch, &br_common, &br_name);

    if (lua_plat_configure(pins, n_pins, cfg->lua_baud, cfg->lua_pixels) != 0) {
        /* Unreachable by the budget in the board platform file. If it ever
         * happens it is a build-time mistake, not an operating condition, so
         * say so plainly rather than degrading quietly. */
        snprintf(status_line, sizeof(status_line), "resource claim failed (firmware bug)");
        return;
    }

    if (want_bridge && lua_plat_configure_bridge(br_ch, br_common, br_name, PYRO_BRIDGE_DEADTIME_CYCLES) != 0) {
        snprintf(status_line, sizeof(status_line), "bridge claim failed (firmware bug)");
        return;
    }

    script_len = lua_app_script_read(script_buf, sizeof(script_buf));
    if (script_len == 0) {
        snprintf(status_line, sizeof(status_line), "enabled, no script (read %s = %d)", LUA_SCRIPT_PATH,
                 script_read_err);
        return;
    }

    /* So a script that cannot match the configuration fails on the bench
     * rather than in the air. */
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

    /* Launching inside init would put a user program between power-on and
     * the first HTTP response, so a program that wedges the board would also
     * block the only route in to replace it. */
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
        /* The one call on this path that touches the FIFO; without the
         * crumbs a hang inside it and after it look identical. */
        flash_window_crumb(60);
        lua_core1_start(script_buf, script_len);
        flash_window_crumb(61);
        phase(17);
        snprintf(status_line, sizeof(status_line), "running (%d out, %d in, %d serial, %d px)",
                 lua_iface_count_kind(LUA_IF_OUTPUT), lua_iface_count_kind(LUA_IF_INPUT),
                 lua_iface_count_kind(LUA_IF_SERIAL), lua_iface_count_kind(LUA_IF_PIXEL));
        phase(18);
        return;
    }

    if (lua_core1_state() == LUA_C1_OFF) {
        /* Launched and not RUNNING means wedged in pyro_lua_init() or
         * pyro_lua_load(). */
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

    /* The seqlock write never waits, so this costs a fixed handful of stores
     * whatever core1 is doing. */
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
    /* [DAT-02, N11] The flight log's time column is flight time, since T+0,
     * for every row: not uptime, which the sample rows beside it do not use. */
    log_drain(flight_elapsed_ms(ctx, now_ms));

    /* Core0 has withheld flash for the whole of startup, so waiting longer
     * costs more than killing core1. */
    if (!lua_core1_ready() && launched && (int32_t)(now_ms - (launch_at_ms + LUA_BOOT_LIMIT_MS)) >= 0 &&
        lua_core1_state() == LUA_C1_RUNNING) {
        snprintf(status_line, sizeof(status_line), "killed: startup exceeded %lums", (unsigned long)LUA_BOOT_LIMIT_MS);
        lua_core1_kill();
    }

    if (lua_core1_state() == LUA_C1_DEAD && strncmp(status_line, "stopped", 7) != 0) {
        phase(PH_KILLED);
        uint32_t ok, rq, ak, hb;
        lua_core1_dispatch_stats(&ok, &rq, &ak, &hb);
        uint32_t loc = lua_core1_loc();
        snprintf(status_line, sizeof(status_line),
                 "stopped: %s [ok=%lu req=%lu ack=%lu hb=%lu loc=%lu failloc=%lu preq=%lu stack=%lu]",
                 lua_core1_error(), (unsigned long)ok, (unsigned long)rq, (unsigned long)ak, (unsigned long)hb,
                 (unsigned long)(loc & 0xff), (unsigned long)((loc >> 8) & 0xff), (unsigned long)((loc >> 16) & 0xff),
                 (unsigned long)lua_core1_stack_free());
    }
}

/* Call after the flash window has closed, with the microseconds left before
 * the deadline. A unit may overrun its box slightly, hence the reserve.
 *
 * Returning without dispatching is normal: core1 stays in RAM for a period,
 * which is what a long flash write needs. */
void lua_app_dispatch(int64_t slack_us) {
    if (!lua_core1_ready()) {
        return; /* still in startup; a grant would mean nothing */
    }
    if (slack_us <= (int64_t)LUA_FLASH_RESERVE_US) {
        return;
    }
    uint32_t grant = (uint32_t)(slack_us - (int64_t)LUA_FLASH_RESERVE_US);
    if (grant > LUA_GRANT_MAX_US) {
        grant = LUA_GRANT_MAX_US;
    }
    if (grant < LUA_GRANT_MIN_US) {
        return;
    }
    lua_core1_dispatch(grant);
}

void lua_app_event(const char *name) {
    lua_core1_event(name);
}
