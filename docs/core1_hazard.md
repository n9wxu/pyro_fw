# Proving the core0 problem

The safety rule for putting Lua on core1 is one sentence:

> **core0 must never wait on anything core1 can hold.**

This document proves the rule is currently violated — not by argument, but by
an experiment anyone can rerun. Nothing here needs a board attached.

## The experiment

Build the same firmware twice. The only difference is one line in the link
list: `pico_multicore`. No source file changes. `multicore_launch_core1()` is
never called.

```sh
cmake -B build-mk1c  -DPYRO_BOARD=mk1c -DCMAKE_BUILD_TYPE=Release
cmake -B build-core1 -DPYRO_BOARD=mk1c -DCMAKE_BUILD_TYPE=Release -DPYRO_LINK_MULTICORE=ON
cmake --build build-mk1c  -j8
cmake --build build-core1 -j8

support/prove_core0.py build-mk1c/pyro_fw_mk1c.elf build-core1/pyro_fw_mk1c.elf
```

### Result

```
=== build-mk1c/pyro_fw_mk1c.elf ===
unbounded primitives linked in : (none)
PASS  no flight-critical root reaches an unbounded wait

=== build-core1/pyro_fw_mk1c.elf ===
unbounded primitives linked in : mutex_enter_blocking

FAIL  action_launch can wait forever on mutex_enter_blocking
      action_launch -> hal_log_start -> lfs_mount -> __wrap_free -> mutex_enter_blocking

FAIL  hal_pyro_update can wait forever on mutex_enter_blocking
      hal_pyro_update -> pyro_update -> hal_fs_open -> lfs_mount -> __wrap_free -> mutex_enter_blocking
```

Three symbols appear in the binary purely from the link line:

```
__mutex_enter_blocking_veneer
malloc_mutex
mutex_enter_blocking
```

## Why one link line does that

`pico/malloc.h` (SDK 2.2.0):

```c
// PICO_CONFIG: PICO_USE_MALLOC_MUTEX, ... default=1 with pico_multicore, 0 otherwise
#if LIB_PICO_MULTICORE && !defined(PICO_USE_MALLOC_MUTEX)
#define PICO_USE_MALLOC_MUTEX 1
#endif
```

`pico_malloc` already wraps `malloc`/`calloc`/`realloc`/`free` on every build
(it is pulled in by `pico_stdlib`). Turning the flag on rewrites all four to:

```c
#define MALLOC_ENTER(outer) mutex_enter_blocking(&malloc_mutex);
```

And `mutex_enter_blocking` (`src/common/pico_sync/mutex.c`) is:

```c
do {
    uint32_t save = spin_lock_blocking(mtx->core.spin_lock);
    if (!lock_is_owner_id_valid(mtx->owner)) { ...; break; }
    lock_internal_spin_unlock_with_wait(&mtx->core, save);
} while (true);
```

No timeout parameter. No failure return. No exit but acquisition. There is no
`mutex_enter_timeout_us` variant reachable here, because the wrapper hardcodes
the blocking call — we cannot pass a deadline even if we want to.

So: **merely enabling core1 converts every heap operation on core0 into an
unbounded wait on a lock core1 also takes.**

## Why it reaches the flight path

`hal_fs_open()` mounts the filesystem on *every* open, and `lfs_mount` →
`lfs_init` → `malloc`, `lfs_unmount` → `free`:

```c
hal_file_t *hal_fs_open(const char *path, bool append) {
    if (hw_file.open) return NULL;
    if (lfs_mount(&hw_file.lfs, &lfs_pico_flash_config) != LFS_ERR_OK) return NULL;
```

and `hal_log_start()` — called from `action_launch`, i.e. **at liftoff** — is
`hal_fs_open("flight_log.csv", false)`.

The comment above the header write says *"first call is not time-critical."*
That assumption is about duration. The hazard is not duration. A core1 that
faults inside `malloc` leaves the mutex owned by a core that will never exit,
and liftoff detection never returns.

`__wrap_malloc`'s only callers in this firmware are `lfs_init` and
`lfs_file_opencfg_`. The printf family does **not** allocate in this build
(checked: no path from `snprintf`/`vfprintf` to `__wrap_malloc`), so the whole
exposure is littlefs. That is a small, fixable surface — see below.

## Second, independent hazard: XIP

`src/littlefs_driver.c` and `src/http_server.c` erase and program flash under:

```c
uint32_t ints = save_and_disable_interrupts();
flash_range_erase(...);
restore_interrupts(ints);
```

`save_and_disable_interrupts()` acts on the **calling core only**. No
`flash_safe_execute`, no `multicore_lockout_start_*` — confirmed by symbol
listing: the multicore build contains `core1_stack` and no lockout machinery at
all.

While that erase runs, XIP is off. Core1 executing from flash fetches garbage.
The likely outcome is a core1 HardFault, whose default handler spins forever —
and if it faults while holding `malloc_mutex`, the two hazards compose into a
permanent core0 hang.

This one is symmetric and it is the reason core0 cannot simply "be careful":
core0's own log flush is what breaks core1.

## Correction to an earlier claim

I previously said both `multicore_lockout_start_blocking` and
`multicore_lockout_start_timeout_us` block core0. That is wrong for the timeout
variant. In SDK 2.2.0 `multicore_lockout_start_block_until` uses
`mutex_enter_block_until` and `multicore_fifo_push/pop_timeout_us`, and returns
`false` on expiry. Only the `_blocking` names pass `at_the_end_of_time`.

The practical consequence is unchanged but the reason differs: with the timeout
variant core0 escapes, it just cannot write flash. That is a degraded-mode
problem, not a hang.

## Is the whole thing just malloc?

The *demonstrated* deadlock is, yes — and it is narrow. Two call sites in
littlefs, removable with `LFS_NO_MALLOC` and four static buffers. If that were
the entire hazard, the honest conclusion would be "fix the malloc path and stop
worrying about core1."

It is not the entire hazard, and the proof above has a blind spot worth stating
plainly, because a green check that cannot fail is worse than no check.

`spin_lock_unsafe_blocking()` is `__force_inline`:

```c
while (__builtin_expect(!*lock, 0)) {
    tight_loop_contents();
}
```

No call, no symbol, nothing for a call-graph analysis to find. It is an
unbounded wait that `prove_core0.py` structurally cannot see. The tool now
finds these by matching the spin lock addresses (`SIO_BASE+0x100..0x17c`) in
literal pools and reports them separately — it never fails on them, because
taking a spin lock is normal and correct.

The SDK states its own safety argument in a comment next to that loop:

> by convention these spin_locks are VERY SHORT LIVED and NEVER BLOCK and run
> with INTERRUPTS disabled ... therefore nothing on our core could be blocking
> us, so we just need to wait on another core anyway which should be finished
> soon

The convention holds only while the other core *finishes*. Today the users are
`hw_claim_*` (spin lock 11) and `irq_*` (spin lock 9), and both run only at
boot, so nothing is at risk yet. That changes when core1 loads PIO programs for
Lua: `pio_claim_unused_sm()` and friends take spin lock 11 at runtime, on core1.

Which produces a deadlock that has nothing to do with malloc, and that **our own
remedy creates**: core0 decides core1 is wedged and does a unilateral PSM
`frce_off`. If core1 was inside `hw_claim_lock` at that instant, spin lock 11 is
now held by a core that no longer exists. Core0's next claim — relaunching
core1, allocating a DMA channel — spins forever, interrupts disabled, no
watchdog feed.

`spin_locks_reset()` unlocks all 32 and is the escape, so the kill sequence has
to be kill, *then reset*, then relaunch — never kill and relaunch. That
ordering is not obvious, and getting it wrong converts the recovery path into
the failure.

So: the malloc mutex is the one hazard proven today and it is cheap to remove.
The class it belongs to — shared SDK state with an unbounded acquire — grows
with every capability core1 gains, and is not something a call graph can settle.

## What this implies for the design

1. **core0 must not allocate.** littlefs has a supported switch for exactly
   this: `LFS_NO_MALLOC`. It requires the caller to supply what the four
   allocation sites would otherwise take from the heap —
   `cfg->read_buffer`, `cfg->prog_buffer`, `cfg->lookahead_buffer`
   (`lfs.c:4275,4286,4303`, in `lfs_init`) and a per-file `buffer` passed to
   `lfs_file_opencfg` (`lfs.c:3193`). Those are the only `__wrap_malloc`
   callers in the firmware, so defining it plus four static buffers removes
   core0's exposure entirely, and the malloc hazard becomes moot regardless of
   what core1 does. Mounting once instead of per-open is worth doing too, but
   it is not sufficient on its own — `lfs_file_open` allocates as well.

2. **Lua must not allocate through the system heap.** Already true — the arena
   in `src/lua/lua_arena.c` exists for this, and is why Lua gets its own heap
   rather than a memory cap on the shared one.

3. **Flash writes must stop core1 first**, using a mechanism that cannot hang
   core0 if core1 is already dead. The lockout handshake needs a cooperative
   core1, so the fallback must be unilateral: PSM `frce_off` on core1, then
   erase, then relaunch. Core0 never waits for consent it might not get.

4. **The core1 kill sequence is `frce_off` -> `spin_locks_reset()` ->
   relaunch.** Killing a core mid-critical-section strands whatever spin lock
   it held; the reset is what makes a unilateral kill survivable.

5. `support/prove_core0.py` is the standing check. It fails the build if any
   flight-critical root can reach an unbounded wait, so that one obligation
   stays proven as the code changes rather than being re-argued. It is not a
   safety case: inlined spin lock acquires and indirect calls are outside what
   it can see, and it says so in its own output.
