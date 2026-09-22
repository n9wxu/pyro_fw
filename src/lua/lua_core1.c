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
/* Park handshake, as a pair of counters rather than a pair of flags.
 *
 * The flag version had a race that bit on the bench. core0 set park_req,
 * waited for park_ack, did its flash write and cleared park_req. core1, on
 * leaving its spin, cleared park_ack -- but not atomically with leaving. So
 * core0's NEXT park could observe the stale park_ack still set, conclude
 * core1 was parked, and erase flash while core1 was on its way back into XIP.
 *
 * With counters there is no stale value to misread: core0 waits for the ack
 * to equal the exact request number it just issued. Odd means "park", even
 * means "released"; core0 owns park_req and core1 owns park_ack. */
static volatile uint32_t park_req; /* core0 writes; odd = park requested */
static volatile uint32_t park_ack; /* core1 writes; mirrors park_req      */
static volatile uint8_t c1_state = LUA_C1_OFF;

/* Where core1 last was. Same idea as the main-loop breadcrumb on core0: when
 * a core stops, the useful question is which call it stopped in, and that is
 * cheaper to record than to deduce. Lives in RAM and is written from RAM on
 * the park path, so it stays valid even with XIP off. */
volatile uint8_t c1_loc;
#define C1_LOC_TOP 1u
#define C1_LOC_PARKCHK 2u
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
static uint32_t park_fail_req, park_fail_ack, park_fail_hb;
static uint8_t park_fail_loc;
static uint32_t park_ok_count;
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
 * RAM-resident function, and the ack is published from inside it. That
 * ordering matters: once core0 observes the ack, core1's program counter is
 * already inside RAM, so there is no window where core0 believes core1 is
 * parked while core1 is still executing from XIP. */
void __not_in_flash_func(lua_core1_park_check)(void) {
    /* Re-evaluate in a loop; do NOT acknowledge whatever park_req happens to
     * hold on the way out.
     *
     * The earlier version left the spin and then did park_ack = park_req. If
     * core0 had already issued the NEXT park by that moment -- and littlefs
     * parks once per sector, back to back -- core1 published an ack for a
     * request it was not honouring, while it was on its way back into flash.
     * core0 took that ack at face value and erased with core1 live in XIP,
     * which hangs both cores on the stalled fetch. On the bench that showed
     * up as core0 dying in hal_tasks_tick and core1 frozen with ack stuck at
     * the previous request.
     *
     * Here core1 returns to flash only after observing an EVEN park_req and
     * acknowledging that. A new odd request arriving at any point is seen on
     * the next turn of this loop and keeps core1 in RAM, so an odd ack means
     * core1 is parked, always. */
    for (;;) {
        uint32_t req = park_req;
        if (!(req & 1u)) {
            if (park_ack != req) {
                __dmb();
                park_ack = req;
            }
            c1_state = LUA_C1_RUNNING;
            c1_loc = C1_LOC_UNPARKED;
            __dmb();
            return; /* released, and only now is flash safe to touch */
        }
        c1_state = LUA_C1_PARKED;
        c1_loc = C1_LOC_PARKED;
        __dmb();
        park_ack = req;
        while (park_req == req) {
            tight_loop_contents();
        }
    }
}

bool lua_core1_park(uint32_t timeout_us) {
    if (c1_state == LUA_C1_OFF || c1_state == LUA_C1_DEAD) {
        return true; /* nothing running to park */
    }
    uint32_t req = (park_req + 1u) | 1u; /* next odd number */
    park_req = req;
    __dmb();
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (park_ack != req) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            /* Core1 did not answer. It may be in a long C function, stuck in
             * a peripheral loop, or gone. Core0 does not investigate and does
             * not wait: it removes core1 and carries on. */
            park_fail_req = req;
            park_fail_ack = park_ack;
            park_fail_hb = heartbeat;
            park_fail_loc = c1_loc;
            start_err = "no answer to a park request";
            lua_core1_kill();
            return false;
        }
        tight_loop_contents();
    }
    park_ok_count++;
    return true;
}

void lua_core1_park_stats(uint32_t *ok, uint32_t *req, uint32_t *ack, uint32_t *hb) {
    *ok = park_ok_count;
    *req = park_fail_req;
    *ack = park_fail_ack;
    *hb = park_fail_hb;
}

uint32_t lua_core1_loc(void) {
    return c1_loc | ((uint32_t)park_fail_loc << 8) | ((uint32_t)park_req << 16);
}

void lua_core1_unpark(void) {
    if (park_req & 1u) {
        park_req = park_req + 1u; /* even: released */
        __dmb();
    }
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
    /* NO spin_locks_reset() here.
     *
     * It was here as defence in depth against core1 dying inside a spin lock
     * critical section. But prove_core0.py --core1 shows core1 reaches no
     * acquire at all, so there is nothing to rescue -- and the call is
     * actively harmful on this path: it unlocks all 32 spin locks, and core0
     * runs this with interrupts ENABLED. A USB, timer or UART handler holding
     * a striped spin lock at that instant would have it yanked out from under
     * it. Rescuing a hazard that cannot happen, by creating one that can, is
     * a bad trade. */
    /* Core1 is off and cannot write a register again, so core0 is now the only
     * writer and can put its outputs down without coordinating with anything.
     *
     * Stopping the VM is not the same as stopping what it was driving. A pin
     * left high stays high, and Lua's PIO state machines are free-running by
     * design, so they keep clocking a pad after the core that started them is
     * gone. On a board where a Lua pin is a logic-level header that is a stuck
     * LED; on one where it drives a power FET it is a solenoid held for the
     * rest of the flight. Core0 is the safety core, so core0 clears it. */
    lua_plat_safe_outputs();

    c1_state = LUA_C1_DEAD;
    park_req = park_ack; /* no outstanding request against a core that is gone */
    __dmb();
}

/* ── core1 entry ──────────────────────────────────────────────────── */

static void core1_main(void) {
    c1_loc = C1_LOC_INIT;
    pyro_lua_init();
    c1_loc = C1_LOC_LOAD;
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
        c1_loc = C1_LOC_PARKCHK;
        lua_core1_park_check();
        c1_loc = C1_LOC_PINSVC;
        lua_plat_pin_service();

        if (evt_seq != seen) {
            seen = evt_seq;
            c1_loc = C1_LOC_EVENT;
            pyro_lua_event(evt_name);
            evt_ack = seen;
        }
        c1_loc = C1_LOC_TICK;
        pyro_lua_tick();
        c1_loc = C1_LOC_TOP;
    }
}

/* ── core0 side ───────────────────────────────────────────────────── */

/* ── core1's stack ────────────────────────────────────────────────
 *
 * The SDK's default core1 stack is PICO_CORE1_STACK_SIZE = 0x800, two
 * kilobytes, placed in SCRATCH_X at 0x20040000. Lua does not fit in that:
 * lparser.c is recursive descent and the VM nests C frames per call. Worse,
 * overflowing a stack at 0x20040000 runs DOWN into 0x2003ffff, which is the
 * top of main SRAM -- where core0's own stack lives. So a core1 stack
 * overflow does not crash core1, it silently destroys core0, and the symptom
 * is the whole board going quiet with USB still enumerated.
 *
 * That is exactly what happened on the bench: the LED blinked for about ten
 * seconds and then stopped.
 *
 * So core1 gets its own stack here, in bss, well clear of core0's, with a
 * guard word at the low end that core0 checks. Overflow now corrupts our own
 * guard and is reported, instead of corrupting the core that flies the
 * rocket. */
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
    script_src = script;
    script_len = len;
    con_head = con_tail = 0;

    for (uint32_t i = 0; i < C1_STACK_WORDS; i++) {
        c1_stack[i] = C1_GUARD;
    }

    /* Put core1 into a known state FIRST.
     *
     * multicore_launch_core1_raw() is a push/pop handshake with no timeout
     * (pico-sdk 2.2.0, multicore.c: `do { ... multicore_fifo_pop_blocking();
     * } while (seq < count_of(cmd_sequence))`). It only terminates if core1
     * is sitting in the bootrom answering. "core1 is cold at boot" turned out
     * to be false on this board: after an OTA or a watchdog reboot, core0
     * restarts while core1 may still be running the PREVIOUS firmware, so it
     * never answers and core0 hangs inside the launch. On the bench that
     * showed up as stage 6 at 2068 ms -- core0 dying at the exact millisecond
     * it tried to start core1.
     *
     * The SDK's multicore_reset_core1() does this correctly but then calls
     * multicore_fifo_pop_blocking() to wait for the resurrected core to
     * announce itself, which is another unbounded wait on an untrusted core.
     * So do the PSM half by hand -- both loops below read back our own write
     * and cannot hang -- and let the launch handshake, which already drains
     * the FIFO before its first command, provide the synchronisation. */
    hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (!(psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS)) {
        tight_loop_contents();
    }
    hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while (psm_hw->frce_off & PSM_FRCE_OFF_PROC1_BITS) {
        tight_loop_contents();
    }
    multicore_fifo_drain();
    multicore_fifo_clear_irq();

    multicore_launch_core1_with_stack(core1_main, c1_stack, sizeof(c1_stack));
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

void lua_core1_check_stack(void) {
    if (c1_state == LUA_C1_RUNNING && !lua_core1_stack_ok()) {
        /* The guard is gone, so core1 has already written below its stack.
         * Stop it now: the next few words down belong to us. */
        start_err = "core1 stack overflow";
        lua_core1_kill();
    }
}
