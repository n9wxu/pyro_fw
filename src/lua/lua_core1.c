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
#include "hardware/structs/watchdog.h"
#include <string.h>

/* ── Shared state ─────────────────────────────────────────────────── */

static volatile uint32_t heartbeat; /* core1 writes */
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

/* ── Byte rings, core1 -> core0 ───────────────────────────────────
 *
 * Single producer (core1), single consumer (core0), power-of-two size, so
 * head and tail are each written by one core only and no lock is needed.
 *
 * Two of them: the console, which core0 serves to the web UI, and the log,
 * which core0 appends to a file. They are the same structure because they are
 * the same problem -- core1 produces bytes it must not block on and must not
 * write to flash itself.
 *
 * Full means drop. That is deliberate: the alternative is core1 waiting on
 * core0, which is the one thing this whole module exists to prevent. Silent
 * dropping is not acceptable though, so each ring counts what it lost and
 * /api/status reports it. */

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

/* Core1 hands log bytes over; core0 owns the file. Core1 never touches
 * flash, so this is the only way a script can reach the log. */
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
 * core0 owns go and grant_us; core1 owns seen and busy. One writer each, so
 * no lock and nothing to contend.
 *
 * busy is the whole flash interlock. It is set from RAM before core1 returns
 * to flash and cleared from RAM after it comes back, so core0 observing
 * busy == 0 means core1's program counter is genuinely in RAM -- not merely
 * that it promised to go there. */
static volatile uint32_t c1_go;       /* core0 bumps to hand out a unit  */
static volatile uint32_t c1_seen;     /* core1 mirrors when it takes one */
static volatile uint32_t c1_grant_us; /* core0 writes before bumping go  */
static volatile uint8_t c1_busy;      /* core1: 1 = executing from flash */

/* Set by core1 the first time it reaches the RAM-resident dispatch loop,
 * i.e. once pyro_lua_init(), the chunk body and init() have all finished AND
 * its program counter has actually left flash. Declared here because
 * lua_core1_idle_wait() below is where it is published. */
static volatile uint8_t c1_ready;
static uint32_t dispatch_skipped; /* grants dropped: core1 overran   */

/* core1's idle: publish idle, wait for work, claim it, declare busy.
 *
 * RAM-resident, and the ordering is the point. Clearing busy happens here,
 * after core1 has left flash; setting it happens here too, before it goes
 * back. Neither transition is visible to core0 while core1 is somewhere it
 * should not be. */
/* __noinline is load-bearing, not a hint.
 *
 * __not_in_flash_func() only attaches a section attribute. It does NOT stop
 * the compiler inlining the body into its caller, and core1_main() -- the
 * single call site -- lives in .text, in flash. GCC duly inlined this, the
 * out-of-line copy was elided entirely, and the symbol vanished from the
 * binary: core1's "RAM-resident" idle spin was executing from XIP.
 *
 * Which is the whole deadlock. Core1 sat in a tight loop issuing flash reads
 * while core0 took XIP down and drove the SSI directly; the bootrom's
 * flash_wait_ready() then polled a status register it could never read
 * correctly and never returned. On the bench that is core0 stopped inside
 * flash_range_program(), watchdog at 1000 ms, "died in stage 97" -- and it
 * reproduced only with Lua running, because core1 is the only thing that
 * fetches from flash while core0 is erasing.
 *
 * support/prove_core0.py fails the build if the symbol goes missing again. */
void __noinline __not_in_flash_func(lua_core1_idle_wait)(void) {
    c1_busy = 0;
    c1_state = LUA_C1_PARKED;
    c1_loc = C1_LOC_PARKED;
    /* Startup is over, published from RAM.
     *
     * It used to be published at the end of core1_main's prologue, right
     * after pyro_lua_load() returned -- from FLASH, with the store itself,
     * the barrier, the evt_seq read and the call into this function all
     * still to be fetched over XIP. Core0 reads c1_ready as "core1 is out of
     * flash and the window may open", so for those few instructions core0
     * believed something core1 could not yet back, opened the window and
     * started an erase into a core1 that was mid-fetch. On the bench that
     * was the board dying in the flash window at 2097 ms, every boot.
     *
     * c1_busy already followed this rule; c1_ready did not, and it is the
     * same rule for the same reason. The whole point of a RAM-resident
     * function is that a flag set inside it is a fact about core1's program
     * counter rather than a promise about where it is going. */
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
        /* Still on the previous unit. Skip rather than queue: a grant that
         * piles up is a core1 running further and further behind core0, and
         * the counter says so out loud. */
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

/* c1_busy alone is not enough, and the gap is small but real.
 *
 * lua_core1_dispatch() bumps c1_go; core1 notices, claims the unit by
 * mirroring it into c1_seen, and only then sets c1_busy. Between those two
 * points a grant is outstanding and core1 is about to enter flash, while
 * c1_busy still reads 0. Core0 asking "is core1 idle" in that gap would get
 * yes, open the window, and start an erase into a core1 that is a handful of
 * instructions away from fetching.
 *
 * In practice core1's idle spin closes the gap in nanoseconds and core0 does
 * not come back round for a whole period. But "the race is narrow" is not
 * the same as "there is no race", and an outstanding grant is something
 * core0 can see for free: it is the one that issued it.
 *
 * So: idle means no grant outstanding AND not executing. */
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

/* Same question, same answer. Kept as a separate name because the header
 * asks it two ways -- "is flash safe" and "is core1 idle" -- and they must
 * never drift apart: core1's unbounded startup is a stretch where it is not
 * idle, so a version of this without the c1_ready check would say yes while
 * core1 was running the Lua parser out of XIP. */
bool lua_core1_idle(void) {
    return lua_core1_flash_ok();
}

uint32_t lua_core1_dispatch_skipped(void) {
    return dispatch_skipped;
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
    __dmb();
}

/* Sub-phase breadcrumb inside the launch. Same scratch register and tag
 * lua_app.c uses, so a watchdog reboot reports it as "(phase N)" with no new
 * plumbing. RAM does not survive the reset -- PSM_WDSEL covers sram0-5 -- but
 * the watchdog scratch registers do, which is why the breadcrumbs live there. */
#define LAUNCH_PHASE(n) (watchdog_hw->scratch[2] = 0x50480000u | (uint32_t)(n))

/* ── core1 entry ──────────────────────────────────────────────────── */

static void core1_main(void) {
    /* Breadcrumbs from CORE1, into the same watchdog scratch register core0
     * uses. RAM does not survive a watchdog reset but the scratch registers
     * do, so this is the only way to see how far core1 got before it took the
     * board down. 20+ to stay clear of core0's phases. */
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

    /* c1_ready is NOT set here. Everything between this point and the first
     * instruction of lua_core1_idle_wait() is still fetched from flash, and
     * core0 treats c1_ready as permission to erase. It is published from
     * inside that RAM-resident function instead. */
    uint8_t seen = evt_seq;
    while (1) {
        /* Idle in RAM until core0 hands out a unit. EVERYTHING BELOW THIS
         * LINE runs from flash, and core0 knows it because core0 started it --
         * which is what makes core0's own flash writes safe without asking. */
        lua_core1_idle_wait();

        heartbeat++;
        c1_loc = C1_LOC_PINSVC;
        lua_plat_pin_service();

        /* The event handler shares the unit's box with tick(). It cannot
         * yield, so it is bounded by raising rather than by suspending, and
         * whatever it spends comes out of what tick() gets -- which is the
         * correct accounting: the grant is a statement about how long core1
         * holds c1_busy, not about how it divides the time up. */
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
    LAUNCH_PHASE(10);
    script_src = script;
    script_len = len;
    ring_console.head = ring_console.tail = 0;
    ring_log.head = ring_log.tail = 0;

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

    /* Wait for core1 to announce itself, and CONSUME the announcement.
     *
     * This step is why the hand-rolled reset above is not simply the PSM half
     * of multicore_reset_core1(). Out of reset, core1's bootrom drains its own
     * mailbox and then pushes a 0 to say it is ready -- the SDK's reset pops
     * that token explicitly.
     *
     * Draining instead, as this did, races it: the drain runs before core1 has
     * pushed, the stray 0 arrives during multicore_launch_core1_raw()'s
     * handshake, and that handshake desynchronises. Its loop is
     * push_blocking/pop_blocking with no timeout, so core0 then spins there
     * forever. On the bench that was the whole board dying about 100 ms after
     * the launch, on every board, with the safe-boot latch reporting "died in
     * stage 6 (phase 3)".
     *
     * The pop is bounded, because an unbounded wait on core1 is the one thing
     * this module may not do. If the token never comes, core1 is not in the
     * bootrom and launching would hang -- so do not launch. */
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

    /* Published BEFORE the launch, not from core1 once it is running.
     *
     * lua_core1_park() returns true immediately for LUA_C1_OFF, so while
     * core1 was still in pyro_lua_init()/pyro_lua_load() -- executing the Lua
     * parser from XIP -- core0 believed there was nothing to park and was free
     * to erase flash underneath it. Marking it RUNNING here closes that
     * window: a park during the load now waits for an ack, times out, and
     * kills core1. A lost script, rather than a corrupted filesystem. */
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

/* Watchdog on core1's liveness. A stalled heartbeat means the VM is stuck
 * somewhere the instruction hook cannot reach -- a C function in a long loop,
 * or a peripheral wait. Core0 kills it rather than waiting to find out. */
#define C1_STALL_MS 2000u

/* Kept for the status line's shape. The park protocol is gone, so these
 * report the dispatch counters: units handed out, units taken, and grants
 * skipped because core1 was still working on the previous one. */
void lua_core1_park_stats(uint32_t *ok, uint32_t *req, uint32_t *ack, uint32_t *hb) {
    *ok = c1_go;
    *req = c1_seen;
    *ack = dispatch_skipped;
    *hb = heartbeat;
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

    /* Parked is the normal state for a core1 core0 has deliberately not fed:
     * the flash window parks it for a whole period routinely and an upload
     * parks it for as long as the transfer lasts. So a parked core1 with no
     * grant outstanding is not late -- it is waiting, correctly, and the
     * clock has to keep moving or the first observation after a hold would
     * see a two-second-old timestamp and kill a core1 that did nothing.
     *
     * A grant OUTSTANDING is the opposite. Core0 bumped c1_go and core1 has
     * not mirrored it into c1_seen, so core1 is not spinning in the idle
     * loop where it would have noticed within nanoseconds. It is wedged, and
     * the window can never open again while it is -- c1_go != c1_seen makes
     * lua_core1_flash_ok() false forever. That is a silent, permanent loss
     * of flash, and it is exactly what the bench showed: heartbeat frozen at
     * zero, skips climbing, opens stuck, and nothing reporting why. */
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
        /* The guard is gone, so core1 has already written below its stack.
         * Stop it now: the next few words down belong to us. */
        start_err = "core1 stack overflow";
        lua_core1_kill();
    }
}
