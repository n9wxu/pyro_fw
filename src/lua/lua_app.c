/*
 * Application glue for Lua. See lua_app.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_app.h"
#include "hal.h"
#include "flash_op.h"
#include "hal_storage.h"
#include "flight_events.h"
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
 * reads the marker.
 *
 * Scratch 4..7 belong to the SDK -- watchdog_enable() writes
 * WATCHDOG_NON_REBOOT_MAGIC to scratch[4] and watchdog_reboot() puts its
 * vector in 4..7 -- and 0..1 hold the flight task's stage, so the marker
 * takes 3 and the phase 2. */
#define LUA_BOOT_MARK 0x4C554131u /* "LUA1" */
#define LUA_BOOT_SCRATCH 3
#define LUA_PHASE_SCRATCH 2
#define LUA_SETTLE_MS 15000u

/* From the first flight period. Keeps a user program out of the window
 * between power-on and the first HTTP response, which is the only route in
 * to replace a program that wedges the board. */
#define LUA_LAUNCH_DELAY_MS 2000u

/* One tick()'s time box, events included. The Lua task shares core1 with the
 * net and storage tasks a tick at a time, so this bounds how long one script
 * step holds its share, not the flight. */
#define LUA_TICK_BUDGET_US 5000u

/* Covers the start-up check, compiling the script and running init(). A
 * script that never finishes starting holds its share of core1 for nothing. */
#define LUA_BOOT_LIMIT_MS 5000u

/* How long both sides of a half-bridge are held off between transitions, in
 * PIO cycles at 125 MHz -- 250 cycles is 2 us. A FET turns off in finite
 * time, so this gap is what keeps a momentarily conducting pair from becoming
 * a shoot-through across VBAT. Generous for the parts these boards fit. */
#define PYRO_BRIDGE_DEADTIME_CYCLES 250u

/* Survives a watchdog reboot, so the next boot can report what the flight
 * task was doing. */
#define PH_NO_LUA 1u
#define PH_PRELAUNCH 2u
#define PH_LAUNCHED 3u
#define PH_SETTLED 4u
#define PH_KILLED 5u
#define PH_STARTED 17u
#define PH_REPORTED 18u
#define PHASE_TAG 0x50480000u /* "PH" */
#define STAGE_TAG 0x53540000u /* "ST", main_hardware.c's BOOT() */

static uint32_t boot_phase;
static uint32_t last_stage;
static uint32_t last_stage_ms;

static void phase(uint32_t p) {
    watchdog_hw->scratch[LUA_PHASE_SCRATCH] = PHASE_TAG | p;
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

/* The environment a script will get, from the stored pin assignment. Binds
 * nothing, so the verdict is for the assignment as saved rather than for
 * whatever the running VM holds. */
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
        case LUA_ROLE_BRIDGE: /* the pair is one output to a script */
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
        if (pins[i].name && pins[i].name[0] && env->n < LUA_CHK_MAX_NAMES) {
            strncpy(env->names[env->n++], pins[i].name, LUA_NAME_MAX - 1);
        }
    }

    /* The bridge is configured apart from the pad list, on two released pyro
     * pads; its name still has to resolve. */
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
 * A script's log() output goes into the flight log as event rows, written by
 * the storage task with the samples. Do not open a file here: a second file
 * contends with the flight log for hal_fs_open()'s single streaming handle,
 * which the flight log holds for the whole flight. So logging happens only
 * in flight; on the ground a script's output goes to /api/lua/console. */

/* Fits hal_log_text()'s 80-byte row, of which the widest timestamp plus the
 * ",,,,,,,LUA " prefix take 21 and the newline one. Anything over 57 is
 * truncated there, and only once the flight time reaches ten digits of
 * milliseconds, which is 27 hours of flight. */
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

    lua_pin_cfg_t pins[LUA_CFG_MAX];
    int n_pins = pin_store_lua_pins(pins, LUA_CFG_MAX);

    /* The bridge is wired after the board's own pads: its two pins became
     * available only when configuration released the channel. */
    uint8_t br_ch = 0, br_common = 0;
    const char *br_name = NULL;
    bool want_bridge = pin_store_bridge(&br_ch, &br_common, &br_name);

    int rc = lua_plat_configure(pins, n_pins, cfg->lua_baud, cfg->lua_pixels);
    if (rc == LUA_PLAT_BAD_BAUD) {
        snprintf(status_line, sizeof(status_line), "not started: lua_baud %lu is unusable",
                 (unsigned long)cfg->lua_baud);
        return;
    }
    if (rc != LUA_PLAT_OK) {
        /* Unreachable by the budget in lua_pio_platform.c: a build-time
         * mistake, not an operating condition, so say so plainly. */
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

    uint32_t prev = watchdog_hw->scratch[LUA_PHASE_SCRATCH];
    boot_phase = ((prev & 0xffff0000u) == PHASE_TAG) ? (prev & 0xffffu) : 0u;
    uint32_t st = watchdog_hw->scratch[0];
    last_stage = ((st & 0xffff0000u) == STAGE_TAG) ? (st & 0xffffu) : 0u;
    last_stage_ms = watchdog_hw->scratch[1];

    if (watchdog_hw->scratch[LUA_BOOT_SCRATCH] == LUA_BOOT_MARK) {
        /* Last boot set this and never got far enough to clear it. */
        watchdog_hw->scratch[LUA_BOOT_SCRATCH] = 0;
        snprintf(status_line, sizeof(status_line), "disabled: last boot died in stage %lu at %lu ms (phase %lu)",
                 (unsigned long)last_stage, (unsigned long)last_stage_ms, (unsigned long)boot_phase);
        phase(PH_NO_LUA);
        return;
    }

    /* The script is checked against the bound resources by the Lua task,
     * whose stack is sized for the parser; this one is the 2 kB boot stack. */
    launch_pending = true;
    launch_at_ms = 0;
    phase(PH_PRELAUNCH);
    snprintf(status_line, sizeof(status_line), "starting");
}

void lua_app_restart_commanded(void) {
    watchdog_hw->scratch[LUA_BOOT_SCRATCH] = 0;
}

/* ── Main loop ────────────────────────────────────────────────────── */

static void launch(void) {
    launch_pending = false;
    watchdog_hw->scratch[LUA_BOOT_SCRATCH] = LUA_BOOT_MARK;
    phase(PH_LAUNCHED);
    launched = true;
    flash_op_crumb(60);
    lua_core1_start(script_buf, script_len);
    flash_op_crumb(61);
    phase(PH_STARTED);
    snprintf(status_line, sizeof(status_line), "running (%d out, %d in, %d serial, %d px)",
             lua_iface_count_kind(LUA_IF_OUTPUT), lua_iface_count_kind(LUA_IF_INPUT),
             lua_iface_count_kind(LUA_IF_SERIAL), lua_iface_count_kind(LUA_IF_PIXEL));
    phase(PH_REPORTED);
}

static void publish_flight(const flight_context_t *ctx, uint32_t now_ms) {
    lua_flight_t f;
    f.pressure_pa = ctx->pressure_pa;
    f.altitude_cm = ctx->altitude_cm;
    f.speed_cms = ctx->speed_cms;
    f.max_alt_cm = ctx->max_altitude_cm;
    f.state = (int)ctx->current_state;
    f.time_ms = now_ms;
    for (int i = 0; i < 2; i++) {
        f.pyro[i] = (ctx->channel_ready[i] ? LUA_PYRO_CONTINUITY : 0) |
                    (ctx->fire.channel[i].fired ? LUA_PYRO_FIRED : 0) | (ctx->channel_fault[i] ? LUA_PYRO_FAULT : 0) |
                    (ctx->pyros_armed ? LUA_PYRO_ARMED : 0);
        f.pyro_adc[i] = ctx->channel_adc[i];
    }
    f.under_thrust = ctx->under_thrust ? 1 : 0;
    f.apogee_detected = ctx->apogee_declared ? 1 : 0;
    f.telem_seq = ctx->telemetry_seq;
    lua_core1_publish(&f);
}

/* [LUA-RUN-02] */
static void offer_events(const flight_context_t *ctx) {
    static uint32_t offered;
    uint8_t event;
    while (flight_next_event(ctx, &offered, &event))
        lua_core1_event(flight_event_name(event));
}

void lua_app_service(const flight_context_t *ctx, uint32_t now_ms) {
    if (launch_pending) {
        if (launch_at_ms == 0) {
            launch_at_ms = now_ms + LUA_LAUNCH_DELAY_MS;
        } else if ((int32_t)(now_ms - launch_at_ms) >= 0) {
            launch();
        }
        return;
    }
    if (lua_core1_state() == LUA_C1_OFF) {
        return;
    }

    /* Clear the safe-boot marker once the board has demonstrably survived
     * with the script running. A later crash is then a crash, not a bad boot. */
    if (now_ms > LUA_SETTLE_MS && watchdog_hw->scratch[LUA_BOOT_SCRATCH] == LUA_BOOT_MARK) {
        watchdog_hw->scratch[LUA_BOOT_SCRATCH] = 0;
        phase(PH_SETTLED);
    }

    lua_core1_check_stack();
    publish_flight(ctx, now_ms); /* the seqlock write never waits */
    offer_events(ctx);
    lua_core1_service(now_ms);
    /* [DAT-02] The flight log's time column is flight time, since T+0, for
     * every row, as for the sample rows beside it. */
    log_drain(flight_elapsed_ms(ctx, now_ms));

    const char *refused = lua_core1_refusal();
    if (refused && lua_core1_state() == LUA_C1_RUNNING) {
        snprintf(status_line, sizeof(status_line), "not started: %s", refused);
        lua_core1_kill();
        return;
    }

    if (!lua_core1_ready() && launched && (int32_t)(now_ms - (launch_at_ms + LUA_BOOT_LIMIT_MS)) >= 0 &&
        lua_core1_state() == LUA_C1_RUNNING) {
        snprintf(status_line, sizeof(status_line), "killed: startup exceeded %lums", (unsigned long)LUA_BOOT_LIMIT_MS);
        lua_core1_kill();
    }

    if (lua_core1_state() == LUA_C1_DEAD && !refused && strncmp(status_line, "stopped", 7) != 0) {
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

/* Once a period, after the flight work. Returning without asking is normal
 * while the script is still starting. */
void lua_app_dispatch(void) {
    if (!lua_core1_ready()) {
        return;
    }
    lua_core1_dispatch(LUA_TICK_BUDGET_US);
}
