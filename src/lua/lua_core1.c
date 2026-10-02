/*
 * Lua on core1 -- implementation. See lua_core1.h for the safety argument.
 *
 * Every variable shared with the flight task has one writer. Do not add a
 * mutex, semaphore or queue the flight task would take: it may not wait on
 * anything the Lua task can hold.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_core1.h"
#include "lua_check.h"
#include "lua_platform.h"
#include "lua_platform_cfg.h"
#include "pyro_lua.h"
#include "FreeRTOS.h"
#include "task.h"
#include "rtos_tasks.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "hardware/structs/watchdog.h"
#include <string.h>

/* ── Shared state ─────────────────────────────────────────────────── */

static volatile uint32_t heartbeat;            /* the Lua task writes */
static volatile uint8_t c1_state = LUA_C1_OFF; /* the flight task writes */
static const char *volatile refusal;           /* the Lua task writes, once */

/* Where the Lua task is, for /api/lua/console. The numbers are reported. */
static volatile uint8_t c1_loc;
#define C1_LOC_IDLE 3u
#define C1_LOC_PINSVC 5u
#define C1_LOC_EVENT 6u
#define C1_LOC_TICK 7u
#define C1_LOC_INIT 8u
#define C1_LOC_LOAD 9u

static const char *start_err = "";
static const char *script_src;
static int script_len;

/* ── Flight snapshot: seqlock ─────────────────────────────────────
 *
 * An odd sequence means a write is in progress. A reader that loses the race
 * every time keeps its last consistent copy: stale flight data inconveniences
 * a script, while an unbounded retry would cost the Lua task its liveness. */
#define SNAPSHOT_TRIES 8

static volatile uint32_t flight_seq;
static lua_flight_t flight_data;
static lua_flight_t flight_copy; /* the Lua task's last consistent read */

void lua_core1_publish(const lua_flight_t *f) {
    flight_seq++;
    __dmb();
    flight_data = *f;
    __dmb();
    flight_seq++;
}

const lua_flight_t *lua_flight_snapshot(void) {
    for (int tries = 0; tries < SNAPSHOT_TRIES; tries++) {
        uint32_t s1 = flight_seq;
        if (s1 & 1u) {
            continue;
        }
        __dmb();
        lua_flight_t read = flight_data;
        __dmb();
        if (flight_seq == s1) {
            flight_copy = read;
            break;
        }
    }
    return &flight_copy;
}

/* ── Byte rings, out of the Lua task ──────────────────────────────
 *
 * The Lua task writes head and one reader writes tail -- the net task the
 * console's, the flight task the log's -- so neither needs a lock. A full
 * ring drops rather than blocking the script, and counts what it dropped. */

#define RING_SIZE 2048u
#define RING_MASK (RING_SIZE - 1u)

typedef struct {
    char buf[RING_SIZE];
    volatile uint32_t head;    /* the Lua task writes */
    volatile uint32_t tail;    /* the reader writes */
    volatile uint32_t dropped; /* the Lua task writes */
} c1_ring_t;

static c1_ring_t ring_console;
static c1_ring_t ring_log;

static void ring_put(c1_ring_t *r, const char *s, int len) {
    uint32_t h = r->head;
    int i = 0;
    for (; i < len; i++) {
        uint32_t next = (h + 1u) & RING_MASK;
        if (next == (r->tail & RING_MASK)) {
            break;
        }
        r->buf[h] = s[i];
        h = next;
    }
    __dmb();
    r->head = h;
    if (i < len) {
        r->dropped += (uint32_t)(len - i);
    }
}

static int ring_get(c1_ring_t *r, char *out, int max) {
    int n = 0;
    uint32_t t = r->tail;
    while (n < max && t != r->head) {
        out[n++] = r->buf[t];
        t = (t + 1u) & RING_MASK;
    }
    __dmb();
    r->tail = t;
    return n;
}

void lua_plat_console_out(const char *s, int len) {
    ring_put(&ring_console, s, len);
}

int lua_core1_console_read(char *buf, int max) {
    return ring_get(&ring_console, buf, max);
}

void lua_plat_log_write(const char *s, int len) {
    ring_put(&ring_log, s, len);
}

int lua_core1_log_read(char *buf, int max) {
    return ring_get(&ring_log, buf, max);
}

uint32_t lua_core1_console_dropped(void) {
    return ring_console.dropped;
}

uint32_t lua_core1_log_dropped(void) {
    return ring_log.dropped;
}

/* ── The Lua task [DD-073] ────────────────────────────────────────── */

static TaskHandle_t lua_task_h;
static StaticTask_t lua_task_tcb;
static volatile bool started;
static volatile uint32_t ticks_asked; /* the flight task writes */
static volatile uint32_t ticks_done;  /* the Lua task writes */
static volatile uint32_t tick_budget_us;
static volatile uint8_t c1_busy; /* a tick is running */
static volatile uint8_t c1_ready;
static uint32_t dispatch_skipped;

void lua_core1_dispatch(uint32_t budget_us) {
    if (c1_state != LUA_C1_RUNNING || !c1_ready) {
        return;
    }
    if (c1_busy) {
        /* Skip rather than queue: queued ticks put the script further and
         * further behind the flight. */
        dispatch_skipped++;
        return;
    }
    tick_budget_us = budget_us;
    __dmb();
    ticks_asked++;
    xTaskNotifyGive(lua_task_h);
}

bool lua_core1_ready(void) {
    return c1_ready != 0u;
}

bool lua_core1_flash_ok(void) {
    return true;
}

const char *lua_core1_refusal(void) {
    return refusal;
}

/* vTaskSuspend() stops the task between the kernel's critical sections. What
 * the VM was driving is put down separately: a pin left high stays high, and
 * a PIO state machine keeps clocking a pad after the task is gone. */
void lua_core1_kill(void) {
    if (lua_task_h) {
        vTaskSuspend(lua_task_h);
    }
    lua_plat_safe_outputs();
    c1_state = LUA_C1_DEAD;
    __dmb();
}

/* RAM does not survive a watchdog reset and the scratch registers do. Same
 * register and tag lua_app.c uses, reported as "(phase N)"; numbered from 20
 * to stay clear of the flight task's phases. */
#define LAUNCH_PHASE(n) (watchdog_hw->scratch[2] = 0x50480000u | (uint32_t)(n))

static uint32_t tick_left(uint32_t start_us) {
    uint32_t spent = lua_plat_now_us() - start_us;
    return spent < tick_budget_us ? tick_budget_us - spent : 1u;
}

/* A kilobyte, so static rather than on the task's stack. */
static lua_chk_result_t start_check;

static bool script_matches_resources(void) {
    lua_chk_env_t env;
    lua_chk_env_from_platform(&env);
    pyro_lua_check(script_src, (size_t)script_len, &env, &start_check);
    if (start_check.green) {
        return true;
    }
    refusal = start_check.items[0].detail;
    return false;
}

static void lua_task(void *arg) {
    (void)arg;
    while (!started) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    LAUNCH_PHASE(20);
    if (!script_matches_resources()) {
        c1_ready = 1;
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
    c1_loc = C1_LOC_INIT;
    pyro_lua_init();
    LAUNCH_PHASE(21);
    c1_loc = C1_LOC_LOAD;
    if (!pyro_lua_load("user", script_src, (size_t)script_len)) {
        /* A bad script is not a system failure: the task stays so the
         * console can report the error and the operator can fix it. */
        lua_plat_console_out("load failed: ", 13);
        const char *e = pyro_lua_last_error();
        lua_plat_console_out(e, (int)strlen(e));
        lua_plat_console_out("\n", 1);
    }
    LAUNCH_PHASE(22);
    c1_loc = C1_LOC_IDLE;
    c1_ready = 1;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ticks_done == ticks_asked) {
            continue;
        }
        c1_busy = 1;
        heartbeat++;
        uint32_t start_us = lua_plat_now_us();
        c1_loc = C1_LOC_PINSVC;
        lua_plat_pin_service();
        c1_loc = C1_LOC_EVENT;
        pyro_lua_run_events(tick_left(start_us));
        c1_loc = C1_LOC_TICK;
        pyro_lua_tick_slice(tick_left(start_us));
        c1_loc = C1_LOC_IDLE;
        ticks_done = ticks_asked;
        c1_busy = 0;
    }
}

/* Sized for PYRO_LUA_C_LEVELS (pyro_luaconf.h): 22 levels of 520 B, the
 * matcher's 32 of 40 B and a printf leaf, with margin; the target's
 * high-water mark under the deep-recursion tests is still to be measured.
 * The kernel fills the stack at creation and checks its bottom words at every
 * switch (configCHECK_FOR_STACK_OVERFLOW 2), so nothing of ours may be
 * written there; the stop below acts on the high-water mark, before that
 * check. */
#define C1_STACK_WORDS 4096u
#define C1_STACK_MARGIN_WORDS 16u

static StackType_t c1_stack[C1_STACK_WORDS] __attribute__((aligned(8)));

static bool lua_core1_stack_ok(void) {
    return !lua_task_h || uxTaskGetStackHighWaterMark(lua_task_h) > C1_STACK_MARGIN_WORDS;
}

uint32_t lua_core1_stack_free(void) {
    return lua_task_h ? (uint32_t)uxTaskGetStackHighWaterMark(lua_task_h) * sizeof(StackType_t) : 0u;
}

void lua_task_create(void) {
    lua_task_h = xTaskCreateStaticAffinitySet(lua_task, "lua", C1_STACK_WORDS, NULL, PRIO_P, c1_stack, &lua_task_tcb,
                                              CORE1_ONLY);
}

bool lua_core1_start(const char *script, int len) {
    LAUNCH_PHASE(10);
    if (!lua_task_h) {
        start_err = "no Lua task";
        c1_state = LUA_C1_DEAD;
        return false;
    }
    script_src = script;
    script_len = len;
    ring_console.head = ring_console.tail = 0;
    ring_log.head = ring_log.tail = 0;
    c1_state = LUA_C1_RUNNING;
    __dmb();
    started = true;
    xTaskNotifyGive(lua_task_h);
    LAUNCH_PHASE(16);
    return true;
}

lua_c1_state_t lua_core1_state(void) {
    return (lua_c1_state_t)c1_state;
}

const char *lua_core1_error(void) {
    return start_err;
}

uint32_t lua_core1_heartbeat(void) {
    return heartbeat;
}

void lua_core1_event(const char *name) {
    if (c1_state == LUA_C1_RUNNING) {
        pyro_lua_post_event(name);
    }
}

/* A stalled heartbeat means the VM is stuck where the instruction hook
 * cannot reach: a C function in a long loop, or a peripheral wait. */
#define C1_STALL_MS 2000u

void lua_core1_dispatch_stats(uint32_t *handed, uint32_t *taken, uint32_t *skipped, uint32_t *heartbeat_out) {
    *handed = ticks_asked;
    *taken = ticks_done;
    *skipped = dispatch_skipped;
    *heartbeat_out = heartbeat;
}

uint32_t lua_core1_loc(void) {
    return c1_loc | ((uint32_t)c1_busy << 8) | ((uint32_t)(ticks_asked & 0xffu) << 16);
}

void lua_core1_service(uint32_t now_ms) {
    static uint32_t last_hb;
    static uint32_t last_ms;
    static bool init;

    if (c1_state != LUA_C1_RUNNING) {
        return;
    }

    /* No tick outstanding is normal -- nothing asked for one -- so keep the
     * clock moving, or the first observation after a quiet spell reads a
     * stale timestamp and stops a task that did nothing. */
    if (!c1_busy && ticks_done == ticks_asked) {
        last_hb = heartbeat;
        last_ms = now_ms;
        init = true;
        return;
    }
    if (!init) {
        init = true;
        last_hb = heartbeat;
        last_ms = now_ms;
        return;
    }
    uint32_t hb = heartbeat;
    if (hb != last_hb) {
        last_hb = hb;
        last_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - last_ms) > C1_STALL_MS) {
        start_err = c1_busy ? "Lua stalled in a tick" : "Lua never took its tick";
        lua_core1_kill();
    }
}

void lua_core1_check_stack(void) {
    if (c1_state == LUA_C1_RUNNING && !lua_core1_stack_ok()) {
        start_err = "Lua stack overflow";
        lua_core1_kill();
    }
}
