# Proving the core0 problem

The safety rule for the tasks on core1 -- Lua, the network, the storage
task -- is one sentence:

> **core0 must never wait on anything core1 can hold.**

Under FreeRTOS SMP the flight task is alone at P on core0 (DD-073), and the
rule reads: the flight task never waits on anything another task can hold;
its only blocking call is the wait for its next period
(`src/rtos/rtos_tasks.h`). The Lua task's side of it is in
`src/lua/lua_core1.h`.

This document shows how the rule is broken from underneath the source, by an
experiment anyone can rerun. Nothing here needs a board attached.

## The experiment

Build an image and ask `support/prove_core0.py` two questions of it: whether
any flight root reaches an unbounded wait, and whether a function that
allocates does.

```sh
cmake -B build-mk1c -DPYRO_BOARD=mk1c && cmake --build build-mk1c
support/prove_core0.py --core1 lua_task build-mk1c/pyro_fw_mk1c.elf
support/prove_core0.py --root l_alloc   build-mk1c/pyro_fw_mk1c.elf
```

l_alloc is the Lua library's default allocator: linked, never installed --
both of the firmware's Lua states take `lua_arena_alloc()` -- and so a
function in the image that allocates from the system heap and that no flight
root calls.

### Result

MK1C's image:

```
=== build-mk1c/pyro_fw_mk1c.elf ===
PASS  every flight root is in the image (37)
unbounded primitives linked in : flash_op, mutex_enter_blocking, ...
PASS  no flight-critical root reaches an unbounded wait
...
PASS  lua_task acquires nothing: stoppable at any instant

=== with --root l_alloc ===
FAIL  l_alloc can wait forever on mutex_enter_blocking
      l_alloc
        -> __wrap_realloc
        -> mutex_enter_blocking
```

`mutex_enter_blocking` is in the image though no source file of the firmware
calls it: the heap wrappers bring it.

## Why one link line does that

`pico/malloc.h` (SDK 2.2.0):

```c
// PICO_CONFIG: PICO_USE_MALLOC_MUTEX, ... default=1 with pico_multicore, 0 otherwise
#if LIB_PICO_MULTICORE && !defined(PICO_USE_MALLOC_MUTEX)
#define PICO_USE_MALLOC_MUTEX 1
#endif
```

The SDK's pico_malloc wraps `malloc`/`calloc`/`realloc`/`free` on every
build (it is pulled in by `pico_stdlib`), and the FreeRTOS SMP port links
`pico_multicore` (its `library.cmake`), so the flag is on in every image. It
rewrites all four to:

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

No timeout parameter. No failure return. No exit but acquisition. Under the
FreeRTOS port the wait inside is an event-group wait, which `prove_core0.py`
follows to `xEventGroupWaitBits`. The wrapper hardcodes the blocking call, so
no caller can pass a deadline.

So: **every heap operation, on either core, is an unbounded wait on a lock a
task on the other core can hold.**

## Why it reaches the flight path

Any allocation the flight task makes would put it behind that lock. None
does: in MK1C's image the only caller of `__wrap_realloc` and `__wrap_free`
is l_alloc, and nothing calls `__wrap_malloc`. The printf family does not
allocate in this build: no path leads from `snprintf` or vfprintf to the heap.
The kernel allocates nothing at run time (`configSUPPORT_DYNAMIC_ALLOCATION`
0, `src/rtos/FreeRTOSConfig.h`), alfs uses no memory but what
`src/vfs_flash.c` hands it, and the Lua VM allocates from its own arena
(`src/lua/lua_arena.c`).

The flight task touches no file either: every file call may wait on the
store's lock, which `fs_lock()` refuses to the flight task, and the one file
it reads, the pad marker, comes from RAM through `hal_fs_read_cached()`
(DD-074).

Duration is not the hazard. A task stopped or faulted inside `malloc` leaves
the mutex owned for good, and whoever asks for it next never returns.

## Second, independent hazard: XIP

Programming or erasing the QSPI flash takes it out of execute-in-place.
While it is out, a core fetching from flash fetches garbage, and the likely
outcome is a HardFault whose default handler spins forever.

It reaches the writer too. A core spinning on XIP reads during an erase keeps
the bootrom from reading the flash's status register, and the writer stays
inside the operation until the watchdog resets the board. A loop the compiler
inlined into a flash caller is enough.

`save_and_disable_interrupts()` acts on the calling core only, so the writer
cannot simply "be careful": the other core has to be kept off the flash.

- **On the ground** `flash_op()` raises its caller to T, wakes the other
  core's lockout helper -- a task at T that exists from boot -- which disables
  that core's interrupts and spins in RAM (`flash_op_park`), then runs the
  operation with its own interrupts off (DD-074, `src/flash_op.h`). Every
  wait is bounded: a helper that has not parked in 20 ms refuses the
  operation, and a parked helper frees itself after 1.5 s. `flash_op_park` is
  `__not_in_flash_func` and `__noinline`, and `prove_core0.py` fails an image
  where it has no out-of-line copy in RAM or can reach flash.
- **From launch to landing** the flight task, every interrupt handler core0
  takes, the idle tasks and the kernel paths they take run from RAM
  (FLT-RT-02), which `support/prove_flight_ram.py --strict` proves of every
  image. The log writer's operations run on core1 alone, with core1's
  interrupts off, in pieces inside its slot of core1's frame (FLT-LOG-09,
  `src/flash_slot.h`, `src/core1_frame.h`), and stop nothing on core0
  (FLT-RT-03). Core1's other tasks run from flash and wait while a piece
  lasts. Every other flash operation is refused until landing (DD-104).

## The lockout variants differ

The SDK's multicore lockout comes in two kinds. `multicore_lockout_start_blocking`
waits at_the_end_of_time for the other core to answer. The timeout variant,
in SDK 2.2.0, uses a mutex wait with a deadline and the FIFO's timeout calls,
and returns `false` on expiry: its caller escapes and does not write flash,
which is a degraded mode rather than a hang.

The firmware uses neither. `prove_core0.py` refuses the `_blocking` names on
the flight path, and `flash_op()` parks the other core with its own helper,
every wait bounded.

## malloc is not the whole hazard

The malloc mutex is a narrow hazard: nothing the image runs takes it. The
proof above also has a blind spot, and a green check that cannot fail is
worse than no check.

`spin_lock_unsafe_blocking()` is `__force_inline`:

```c
while (__builtin_expect(!*lock, 0)) {
    tight_loop_contents();
}
```

No call, no symbol, nothing for a call-graph analysis to find. It is an
unbounded wait that `prove_core0.py` structurally cannot see. The tool
finds these by matching the spin lock addresses (`SIO_BASE+0x100..0x17c`) in
literal pools and reports them separately -- it never fails on them, because
taking a spin lock is normal and correct.

The SDK states its own safety argument in a comment next to that loop:

> by convention these spin_locks are VERY SHORT LIVED and NEVER BLOCK and run
> with INTERRUPTS disabled ... therefore nothing on our core could be blocking
> us, so we just need to wait on another core anyway which should be finished
> soon

The convention holds only while the other side *finishes*. The users are
`hw_claim_*` (spin lock 11) and `irq_*` (spin lock 9). Lua drives PIO and
DMA, and `pio_claim_unused_sm()` and friends take spin lock 11 and leave a
claim behind them. A Lua task that claimed at run time would hold, at the
moment it is stopped, whatever it had claimed.

So core1's tasks are kept away from the acquire rather than rescued
afterwards: the flight task claims every pad, PIO state machine, program
offset and DMA channel at boot, and `prove_core0.py --core1` checks that the
Lua task's call graph reaches no `hw_claim_*`, no `irq_*` handler change, no
heap and no flash.

The class this belongs to -- shared SDK state with an unbounded acquire --
grows with every capability core1 gains, and a call graph cannot settle it.

## What this implies for the design

1. **The flight task must not allocate.** Nothing on its path reaches the
   heap wrappers: the kernel's objects are static, alfs takes its memory from
   `src/vfs_flash.c`, and the flight task opens no file.

2. **Lua must not allocate through the system heap.** The arena in
   `src/lua/lua_arena.c` exists for this, and is why Lua gets its own heap
   rather than a memory cap on the shared one.

3. **A flash operation keeps the other core off the flash itself, never by
   consent.** On the ground it parks the other core with a helper that
   exists from boot, every wait bounded (DD-074); from launch to landing the
   flight task's core runs from RAM and the writer's pieces run on core1
   alone, in its slot (DD-104).

4. **The Lua task's stop is unilateral and terminal:** `vTaskSuspend()`,
   then what the VM drove is put down (`lua_plat_safe_outputs()`). There is
   no relaunch: `lua_core1_start()` runs once, since a VM stopped mid-tick
   is not resumable, so nothing reacquires what the task held.

5. `support/prove_core0.py` is the standing check, run in CI over every board
   that builds Lua. It fails if a flight-critical root can reach an unbounded
   wait, if a root is missing from the image, if the Lua task's call graph
   reaches an acquire, if `flash_op_park` has no out-of-line copy in RAM, or
   if the pressure collector's alarm handler (`collector_alarm_isr`, DD-093)
   or `flash_op_park` can reach flash through a callee or a constant. The
   SDK's `i2c_write_blocking` and `i2c_read_blocking` count as unbounded
   waits: a part holding SCL low holds the caller forever. The task ticks
   `hal_tasks_tick()` reaches through a pointer (`pres_tick`,
   `pres_bringup_tick`, `buzzer_tick`) are roots of their own, and a renamed
   root fails rather than drops out (DD-069). Core1's indirect calls are
   followed through tables named `*_vt`, and an image that dispatches
   through a table without one fails (DD-018). It is not a safety case:
   inlined spin lock acquires and other indirect calls are outside what it
   can see, and it says so in its own output.
