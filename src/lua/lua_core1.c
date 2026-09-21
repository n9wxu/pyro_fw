/*
 * Lua on core1 — implementation. See lua_core1.h for the safety argument.
 *
 * Everything shared between the cores lives in this file, is `volatile`, and
 * has exactly one writer. There are no mutexes, no semaphores, no queues and
 * no FIFO waits after launch, because every one of those is a thing core0
 * could end up waiting on. The cost is that the protocols below are
 * hand-rolled; the benefit is that core0's worst case is a bounded poll.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lua_core1.h"
#include "lua_platform.h"
#include "lua_platform_cfg.h"
#include "pyro_lua.h"
#include "hardware/structs/psm.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <string.h>

/* ── Shared state ─────────────────────────────────────────────────── */

static volatile uint32_t heartbeat; /* core1 writes */
static volatile uint8_t park_req;   /* core0 writes */
static volatile uint8_t park_ack;   /* core1 writes */
static volatile uint8_t c1_state = LUA_C1_OFF;
static volatile uint8_t evt_seq; /* core0 writes */
static volatile uint8_t evt_ack; /* core1 writes */
static char evt_name[16];        /* core0 writes before bumping evt_seq */

static const char *start_err = "";
static const char *script_src;
static int script_len;

/* ── Flight snapshot: seqlock ─────────────────────────────────────
 *
 * core0 writes without ever waiting; core1 retries until it reads a stable
 * copy. An odd sequence means a write is in progress. */
static volatile uint32_t flight_seq;
static lua_flight_t flight_data;
static lua_flight_t flight_copy; /* core1's stable read */

void lua_core1_publish(const lua_flight_t *f) {
    flight_seq++; /* odd: write in progress */
    __dmb();
    flight_data = *f;
    __dmb();
    flight_seq++; /* even: consistent */
}

const lua_flight_t *lua_flight_snapshot(void) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s1 = flight_seq;
        if (s1 & 1u) {
            continue;
        }
        __dmb();
        flight_copy = flight_data;
        __dmb();
        if (flight_seq == s1) {
            return &flight_copy;
        }
    }
    /* Bounded: after 8 attempts return the last copy rather than spin. Stale
     * flight data is a correctness nuisance for a user script; an unbounded
     * loop here would be a liveness bug on the core that must stay able to
     * answer a park request. */
    return &flight_copy;
}

/* ── Console ring, core1 -> core0 ─────────────────────────────────
 *
 * Single producer (core1), single consumer (core0), power-of-two size, so
 * head and tail are each written by one core only and no lock is needed. */

#define CON_SIZE 2048u
#define CON_MASK (CON_SIZE - 1u)
static char con_buf[CON_SIZE];
static volatile uint32_t con_head; /* core1 writes */
static volatile uint32_t con_tail; /* core0 writes */

void lua_plat_console_out(const char *s, int len) {
    uint32_t h = con_head;
    for (int i = 0; i < len; i++) {
        uint32_t next = (h + 1u) & CON_MASK;
        if (next == (con_tail & CON_MASK)) {
            break; /* full: drop, never block */
        }
        con_buf[h] = s[i];
        h = next;
    }
    __dmb();
    con_head = h;
}

int lua_core1_console_read(char *buf, int max) {
    int n = 0;
    uint32_t t = con_tail;
    while (n < max && t != con_head) {
        buf[n++] = con_buf[t];
        t = (t + 1u) & CON_MASK;
    }
    __dmb();
    con_tail = t;
    return n;
}

/* ── The park protocol ────────────────────────────────────────────
 *
 * Core1 must not fetch from flash while core0 erases or programs it. The
 * check runs from core1's VM instruction hook; the wait itself is a
 * RAM-resident function, and park_ack is raised as its first statement. That
 * ordering matters: once core0 observes park_ack, core1's program counter is
 * already inside RAM, so there is no window where core0 believes core1 is
 * parked while core1 is still executing from XIP. */
void __not_in_flash_func(lua_core1_park_check)(void) {
    if (!park_req) {
        return;
    }
    c1_state = LUA_C1_PARKED;
    __dmb();
    park_ack = 1;
    while (park_req) {
        tight_loop_contents();
    }
    park_ack = 0;
    c1_state = LUA_C1_RUNNING;
    __dmb();
}

bool lua_core1_park(uint32_t timeout_us) {
    if (c1_state != LUA_C1_RUNNING) {
        return true; /* nothing running to park */
    }
    park_req = 1;
    __dmb();
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!park_ack) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            /* Core1 did not answer. It may be in a long C function, stuck in
             * a peripheral loop, or gone. Core0 does not investigate and does
             * not wait: it removes core1 and carries on. */
            lua_core1_kill();
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

void lua_core1_unpark(void) {
    park_req = 0;
    __dmb();
}

/* ── The kill ─────────────────────────────────────────────────────
 *
 * Deliberately not multicore_reset_core1(): that function brings core1 back
 * out of reset and then calls multicore_fifo_pop_blocking() to wait for it to
 * announce itself -- an unbounded wait on the very core we just decided was
 * untrustworthy. Here core1 goes into reset and stays there.
 *
 * spin_locks_reset() follows because core1 might have been inside a spin lock
 * critical section at the instant it stopped. By design it never takes one
 * (every claim happens on core0 at boot), so this is a second line rather
 * than the first. */
void lua_core1_kill(void) {
    hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) {
        tight_loop_contents(); /* readback of our own write; bounded */
    }
    spin_locks_reset();
    c1_state = LUA_C1_DEAD;
    park_req = 0;
    __dmb();
}

/* ── core1 entry ──────────────────────────────────────────────────── */

static void core1_main(void) {
    pyro_lua_init();
    if (!pyro_lua_load("user", script_src, (size_t)script_len)) {
        /* A bad script is not a system failure: core1 stays alive so the
         * console can report the error and the operator can fix it. */
        lua_plat_console_out("load failed: ", 13);
        const char *e = pyro_lua_last_error();
        lua_plat_console_out(e, (int)strlen(e));
        lua_plat_console_out("\n", 1);
    }
    c1_state = LUA_C1_RUNNING;

    uint8_t seen = evt_seq;
    while (1) {
        heartbeat++;
        lua_core1_park_check();
        lua_plat_pin_service();

        if (evt_seq != seen) {
            seen = evt_seq;
            pyro_lua_event(evt_name);
            evt_ack = seen;
        }
        pyro_lua_tick();
    }
}

/* ── core0 side ───────────────────────────────────────────────────── */

bool lua_core1_start(const char *script, int len) {
    script_src = script;
    script_len = len;
    con_head = con_tail = 0;
    /* The launch handshake has no timeout. It is safe here and only here:
     * core1 is cold out of the bootrom and has not yet run a line of our
     * code. There is no relaunch path for exactly this reason. */
    multicore_launch_core1(core1_main);
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
    if (c1_state != LUA_C1_RUNNING) {
        return;
    }
    if ((uint8_t)(evt_seq - evt_ack) > 0u) {
        return; /* previous event still unconsumed: drop, never block */
    }
    strncpy(evt_name, name, sizeof(evt_name) - 1);
    evt_name[sizeof(evt_name) - 1] = '\0';
    __dmb();
    evt_seq++;
}

/* Watchdog on core1's liveness. A stalled heartbeat means the VM is stuck
 * somewhere the instruction hook cannot reach -- a C function in a long loop,
 * or a peripheral wait. Core0 kills it rather than waiting to find out. */
#define C1_STALL_MS 2000u

void lua_core1_service(uint32_t now_ms) {
    static uint32_t last_hb;
    static uint32_t last_ms;
    static bool init;

    if (c1_state != LUA_C1_RUNNING) {
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
        start_err = "core1 stalled";
        lua_core1_kill();
    }
}
