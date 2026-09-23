/*
 * Lua on core1 — implementation. See lua_core1.h for the safety argument.
 *
 * Every cross-core variable here is volatile with one writer. Do not add a
 * mutex, semaphore, queue or post-launch FIFO wait: core0 could end up
 * waiting on any of them.
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
#include "hardware/structs/watchdog.h"
#include <string.h>

/* ── Shared state ─────────────────────────────────────────────────── */

static volatile uint32_t heartbeat; /* core1 writes */
static volatile uint8_t c1_state = LUA_C1_OFF;

/* Written from RAM on the idle path, so it stays valid with XIP off. */
volatile uint8_t c1_loc;
#define C1_LOC_TOP 1u
#define C1_LOC_PARKED 3u
#define C1_LOC_UNPARKED 4u
#define C1_LOC_PINSVC 5u
#define C1_LOC_EVENT 6u
#define C1_LOC_TICK 7u
#define C1_LOC_INIT 8u
#define C1_LOC_LOAD 9u
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
    /* Stale flight data inconveniences a script; an unbounded retry would
     * cost core1 its liveness. */
    return &flight_copy;
}

/* ── Byte rings, core1 -> core0 ───────────────────────────────────
 *
 * Core1 writes head, core0 writes tail, so neither needs a lock.
 *
 * A full ring drops rather than blocking core1, and counts what it dropped.
 * /api/status reports the count. */

#define RING_SIZE 2048u
#define RING_MASK (RING_SIZE - 1u)

typedef struct {
    char buf[RING_SIZE];
    volatile uint32_t head;    /* core1 writes */
    volatile uint32_t tail;    /* core0 writes */
    volatile uint32_t dropped; /* core1 writes */
} c1_ring_t;

static c1_ring_t ring_console;
static c1_ring_t ring_log;

static void ring_put(c1_ring_t *r, const char *s, int len) {
    uint32_t h = r->head;
    int i = 0;
    for (; i < len; i++) {
        uint32_t next = (h + 1u) & RING_MASK;
        if (next == (r->tail & RING_MASK)) {
            break; /* full: drop the rest, never block */
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

/* ── Dispatch ─────────────────────────────────────────────────────
 *
 * c1_busy is the whole flash interlock. Core1 sets it from RAM before
 * returning to flash and clears it from RAM after coming back, so core0
 * reading zero means core1's program counter is in RAM rather than merely
 * headed there. */
static volatile uint32_t c1_go;       /* core0 bumps to hand out a unit  */
static volatile uint32_t c1_seen;     /* core1 mirrors when it takes one */
static volatile uint32_t c1_grant_us; /* core0 writes before bumping go  */
static volatile uint8_t c1_busy;      /* core1: 1 = executing from flash */

/* Set once pyro_lua_init(), the chunk body and init() have finished AND
 * core1's program counter has left flash. */
static volatile uint8_t c1_ready;
static uint32_t dispatch_skipped; /* grants dropped: core1 overran   */

/* __noinline is load-bearing. __not_in_flash_func() attaches a section
 * attribute and nothing more, so GCC may inline this body into core1_main(),
 * which lives in flash: the out-of-line copy is then elided and core1's idle
 * spin executes from XIP.
 *
 * That deadlocks rather than slowing down. Core1 issues flash reads in a
 * tight loop while core0 has XIP down driving the SSI directly, and the
 * bootrom's flash_wait_ready() polls a status register it cannot read
 * correctly, so core0 never returns from flash_range_program().
 *
 * support/prove_core0.py fails CI if the symbol goes missing. */
void __noinline __not_in_flash_func(lua_core1_idle_wait)(void) {
    c1_busy = 0;
    c1_state = LUA_C1_PARKED;
    c1_loc = C1_LOC_PARKED;
    /* Core0 reads this as permission to erase, so it must be published from
     * RAM. Set anywhere in flash -- including the tail of core1_main's
     * prologue -- it promises where core1 is going instead of stating where
     * core1 is, and core0 erases while core1 is still fetching over XIP. */
    c1_ready = 1;
    __dmb();

    while (c1_go == c1_seen) {
        tight_loop_contents();
    }

    c1_seen = c1_go;
    c1_busy = 1;
    c1_state = LUA_C1_RUNNING;
    c1_loc = C1_LOC_UNPARKED;
    __dmb();
}

void lua_core1_dispatch(uint32_t budget_us) {
    if (c1_state == LUA_C1_OFF || c1_state == LUA_C1_DEAD) {
        return;
    }
    if (c1_busy) {
        /* Skip rather than queue: queued grants put core1 further and
         * further behind core0. */
        dispatch_skipped++;
        return;
    }
    c1_grant_us = budget_us;
    __dmb();
    c1_go++;
}

bool lua_core1_ready(void) {
    return c1_ready != 0u;
}

/* c1_busy alone leaves a gap. Core1 mirrors c1_go into c1_seen to claim a
 * unit and only then sets c1_busy, so in between a grant is outstanding and
 * core1 is about to enter flash while c1_busy still reads zero.
 *
 * The gap closes in nanoseconds and core0 returns only a period later, so the
 * race is narrow rather than absent. Core0 issued the grant, so checking for
 * one costs it nothing. */
static inline bool c1_quiet(void) {
    return c1_go == c1_seen && c1_busy == 0u;
}

bool lua_core1_flash_ok(void) {
    if (c1_state == LUA_C1_OFF || c1_state == LUA_C1_DEAD) {
        return true; /* nothing running to collide with */
    }
    if (!c1_ready) {
        return false; /* startup: core1 is in flash for an unbounded stretch */
    }
    return c1_quiet();
}

/* Do not use multicore_reset_core1(): it brings core1 back out of reset and
 * then calls multicore_fifo_pop_blocking(), an unbounded wait on the core
 * core0 just decided it could not trust. */
void lua_core1_kill(void) {
    hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) {
        tight_loop_contents(); /* readback of our own write; bounded */
    }
    /* Do not add spin_locks_reset() here. It looks like defence against
     * core1 dying inside a critical section, but prove_core0.py --core1 shows
     * core1 reaches no acquire. Meanwhile it unlocks all 32 spin locks with
     * core0's interrupts enabled, so a USB, timer or UART handler holding a
     * striped lock would lose it. */
    /* Stopping the VM does not stop what it was driving: a pin left high
     * stays high, and Lua's PIO state machines are free-running, so they keep
     * clocking a pad after core1 is gone. On a power FET that is a solenoid
     * held for the rest of the flight. */
    lua_plat_safe_outputs();

    c1_state = LUA_C1_DEAD;
    __dmb();
}

/* PSM_WDSEL covers sram0-5, so RAM does not survive a watchdog reset and the
 * scratch registers do. Same register and tag lua_app.c uses, reported as
 * "(phase N)". */
#define LAUNCH_PHASE(n) (watchdog_hw->scratch[2] = 0x50480000u | (uint32_t)(n))

/* ── core1 entry ──────────────────────────────────────────────────── */

static void core1_main(void) {
    /* Numbered from 20 to stay clear of core0's phases. */
    LAUNCH_PHASE(20);
    c1_loc = C1_LOC_INIT;
    pyro_lua_init();
    LAUNCH_PHASE(21);
    c1_loc = C1_LOC_LOAD;
    if (!pyro_lua_load("user", script_src, (size_t)script_len)) {
        /* A bad script is not a system failure: core1 stays alive so the
         * console can report the error and the operator can fix it. */
        lua_plat_console_out("load failed: ", 13);
        const char *e = pyro_lua_last_error();
        lua_plat_console_out(e, (int)strlen(e));
        lua_plat_console_out("\n", 1);
    }
    LAUNCH_PHASE(22);
    c1_state = LUA_C1_RUNNING;

    /* Do not set c1_ready here: everything from this point to the first
     * instruction of lua_core1_idle_wait() is still fetched from flash. */
    uint8_t seen = evt_seq;
    while (1) {
        /* Idle in RAM until core0 hands out a unit. EVERYTHING BELOW THIS
         * LINE runs from flash, and core0 knows it because core0 started it --
         * which is what makes core0's own flash writes safe without asking. */
        lua_core1_idle_wait();

        heartbeat++;
        c1_loc = C1_LOC_PINSVC;
        lua_plat_pin_service();

        /* Shares the unit's box with tick() below. The handler cannot yield,
         * so its bound raises rather than suspends, and what it spends comes
         * out of tick()'s share. */
        uint32_t unit_start_us = lua_plat_now_us();
        if (evt_seq != seen) {
            seen = evt_seq;
            c1_loc = C1_LOC_EVENT;
            pyro_lua_event(evt_name, c1_grant_us);
            evt_ack = seen;
        }
        c1_loc = C1_LOC_TICK;
        uint32_t spent = lua_plat_now_us() - unit_start_us;
        pyro_lua_tick_slice((spent < c1_grant_us) ? (c1_grant_us - spent) : 1u);
        c1_loc = C1_LOC_TOP;
    }
}

/* ── core0 side ───────────────────────────────────────────────────── */

/* The SDK's default core1 stack is two kilobytes in SCRATCH_X at 0x20040000.
 * Lua does not fit -- lparser.c is recursive descent -- and overflowing that
 * stack runs down into 0x2003ffff, the top of main SRAM, where core0's stack
 * lives. Such an overflow destroys core0 rather than crashing core1, and the
 * board goes quiet with USB still enumerated.
 *
 * This one is in bss, clear of core0's, with a guard word core0 checks. */
#define C1_STACK_WORDS 3072u /* 12 kB */
#define C1_GUARD 0xC0DEFACEu

static uint32_t c1_stack[C1_STACK_WORDS] __attribute__((aligned(8)));

bool lua_core1_stack_ok(void) {
    return c1_stack[0] == C1_GUARD && c1_stack[1] == C1_GUARD;
}

uint32_t lua_core1_stack_free(void) {
    /* Words still holding the fill pattern, from the bottom up. */
    uint32_t i = 0;
    while (i < C1_STACK_WORDS && c1_stack[i] == C1_GUARD) {
        i++;
    }
    return i * 4u;
}

bool lua_core1_start(const char *script, int len) {
    LAUNCH_PHASE(10);
    script_src = script;
    script_len = len;
    ring_console.head = ring_console.tail = 0;
    ring_log.head = ring_log.tail = 0;

    for (uint32_t i = 0; i < C1_STACK_WORDS; i++) {
        c1_stack[i] = C1_GUARD;
    }

    /* multicore_launch_core1_raw() is a push/pop handshake with no timeout
     * (pico-sdk 2.2.0, multicore.c: `do { ... multicore_fifo_pop_blocking();
     * } while (seq < count_of(cmd_sequence))`), terminating only when core1
     * sits in the bootrom answering.
     *
     * Core1 is not reliably cold here: after an OTA or a watchdog reboot,
     * core0 restarts while core1 may still run the previous firmware. The
     * two loops below read back core0's own write and cannot hang. */
    LAUNCH_PHASE(11);
    hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) {
        tight_loop_contents();
    }
    LAUNCH_PHASE(12);
    hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS) {
        tight_loop_contents();
    }

    /* Out of reset, core1's bootrom drains its own mailbox and pushes a 0.
     * Pop that token explicitly: draining the FIFO instead races it, the
     * stray 0 arrives during multicore_launch_core1_raw()'s handshake, and
     * that handshake desynchronises into an untimed spin.
     *
     * No token means core1 is not in the bootrom, so the launch would hang.
     * Do not launch. */
    LAUNCH_PHASE(13);
    uint32_t token = 0;
    if (!multicore_fifo_pop_timeout_us(10000, &token) || token != 0u) {
        start_err = "core1 did not announce itself after reset";
        c1_state = LUA_C1_DEAD;
        return false;
    }
    LAUNCH_PHASE(14);
    multicore_fifo_drain();
    multicore_fifo_clear_irq();

    /* Core0 treats LUA_C1_OFF as "no second core to collide with", so
     * leaving it OFF across pyro_lua_init() and pyro_lua_load() would let
     * core0 erase while core1 runs the Lua parser from XIP. */
    c1_state = LUA_C1_RUNNING;
    __dmb();

    LAUNCH_PHASE(15);
    multicore_launch_core1_with_stack(core1_main, c1_stack, sizeof(c1_stack));
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

/* A stalled heartbeat means the VM is stuck where the instruction hook
 * cannot reach: a C function in a long loop, or a peripheral wait. */
#define C1_STALL_MS 2000u

void lua_core1_dispatch_stats(uint32_t *handed, uint32_t *taken, uint32_t *skipped, uint32_t *heartbeat_out) {
    *handed = c1_go;
    *taken = c1_seen;
    *skipped = dispatch_skipped;
    *heartbeat_out = heartbeat;
}

uint32_t lua_core1_loc(void) {
    return c1_loc | ((uint32_t)c1_busy << 8) | ((uint32_t)(c1_go & 0xffu) << 16);
}

void lua_core1_service(uint32_t now_ms) {
    static uint32_t last_hb;
    static uint32_t last_ms;
    static bool init;

    if (c1_state == LUA_C1_OFF || c1_state == LUA_C1_DEAD) {
        return;
    }

    /* Parked with no grant outstanding is normal -- an upload parks core1
     * for the length of the transfer -- so keep the clock moving, or the
     * first observation after a hold reads a two-second-old timestamp and
     * kills a core1 that did nothing.
     *
     * A grant outstanding is the opposite: core1 is not in the idle loop,
     * where it would notice within nanoseconds. c1_go != c1_seen then holds
     * lua_core1_flash_ok() false forever, losing flash permanently. */
    if (c1_state == LUA_C1_PARKED && c1_go == c1_seen) {
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
        start_err = (c1_go != c1_seen) ? "core1 never took its unit" : "core1 stalled";
        lua_core1_kill();
    }
}

void lua_core1_check_stack(void) {
    if (c1_state == LUA_C1_RUNNING && !lua_core1_stack_ok()) {
        /* Core1 has already written below its stack; the next words down
         * belong to core0. */
        start_err = "core1 stack overflow";
        lua_core1_kill();
    }
}
