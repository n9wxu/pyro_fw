# Flight-log storage, plan 2: FreeRTOS SMP

**Status: design, 2026-09-28. Nothing here is implemented.**

Plan 1 is `docs/log_storage_options.md`: R1 keeps the flight path running
from RAM through a flash write, and the log's medium is chosen by its
conflict class. This is a second plan, which you asked for:

- **The flight code** becomes a high-priority task that yields on a schedule.
- **Lua** becomes a lower-priority task that runs when it can; FreeRTOS's
  SMP support lets both run at once.
- **A logging task** has the highest priority, accepts data from the other
  tasks, and blocks them while it works.
- **When it is time to write the flash**, the logging task blocks the whole
  system, does a fully blocking write, and resumes it.
- **Its advantage:** it eases a move to the ESP32.

Figures are cited to `docs/datasheets/` (file and page), to source files
at a named version, or to a named external document. Measurements say how
they were made. Estimates are marked as such.

---

## 1. Summary

The plan works on today's boards, with three conditions. Each is explained
in the section named.

1. **In flight, a write is one page program, never an erase (section 5).**
   A 256-byte page program takes 0.4 ms typically and 3 ms at most. A 4 KB
   sector erase takes 45 ms typically and 400 ms at most (W25Q16JV and
   W25Q128JV, below). A fully blocking write stops both cores and every
   interrupt for its whole duration, so the flight log has to be written as
   pages into a region erased before launch. That is plan 1's RAW-INT
   backend. Written through littlefs as it is today, the log erases flash
   about once a second in flight (L2), and each erase would stop everything
   for up to 400 ms.
2. **One priority for everything but the logger (section 4).** At your
   direction the kernel runs one priority level at a time
   (`configRUN_MULTIPLE_PRIORITIES` 0, FreeRTOS's default). The flight
   task and Lua share a priority, pinned to different cores, so they run
   together; when the logger runs, both stop. Three rules come with it:
   the logger sits at `configMAX_PRIORITIES - 1`, the SDK's lockout
   helper's priority, or a write deadlocks the board; the network task
   shares Lua's core and priority, since anything below that priority
   starves while a script runs; and nothing may depend on the logger
   excluding Lua, because ESP-IDF does not schedule that way.
3. **The flight task never waits on another task (section 8).** It hands
   records over through a lock-free ring and wakes the logger with a
   notification. Its only blocking call is the wait for its next period.

What it costs and what it removes:

- **Cost.** 15.6 KB of code and 5.0 KB of RAM for the kernel, measured
  (section 9). MK1B's application slot has 33.5 KB free today.
- **The flight code pauses during a write**, where R1's keeps running.
  At the densest logging plan that is about 5 pauses a second, of 0.4 ms
  typically and 3 ms at most.
- **Removed from today's code:** the flash window, core1's time-boxed units
  and RAM-resident idle loop, and the core1 kill. R1's RAM-resident flight
  path is not needed either.
- **The ESP32 move.** A flash write under ESP-IDF already works this way:
  the other core is parked, and only interrupts in IRAM run. The task
  structure, the priorities and the logging design carry over; the calls
  that create and pin tasks are renamed (section 11).

---

## 2. What today's design does, and what each part becomes

| Today | Where | Under plan 2 |
|---|---|---|
| One superloop on core0, 20 ms (DD-065); stages 1-8, then slack spent on USB, lwIP and HTTP | `src/main_hardware.c` | The flight task, and a network task for the slack work |
| Core1 idles in a RAM loop and runs Lua and HTTP work only as time-boxed units core0 grants | `src/lua/lua_core1.c`, `lua_core1.h` | A Lua task, preempted like any other; no units, no grants, no RAM idle loop |
| The flash window: flash operations only in STAGE 7 or the slack, while core1 idles in RAM | `src/flash_window.c`, `src/littlefs_driver.c` | The logging task's lockout: `flash_safe_execute()` parks the other core wherever it is |
| Log records copied into a 4 KB buffer on the flight path; the buffer written to littlefs in the window | `src/hal_common/hal_common.c`, `log_keep()` / `log_flash_service()` | A single-producer ring; the logging task drains it into pre-erased pages |
| The core1 kill: PSM `frce_off`, unilateral and terminal | `lua_core1_kill()` | Removed: under SMP FreeRTOS, core1 runs the scheduler too (section 7) |
| `prove_core0.py`: no flight root calls a wait with no timeout | `support/prove_core0.py` | The same, with the kernel's calls classified (section 8) |
| `wait_check.py`: no sleeps; every wait a deadline (DD-053) | `support/wait_check.py` | The flight task's one blocking call is the wait for its next period |

---

## 3. The tasks

Two priority levels are used, and nothing between them: the top,
`configMAX_PRIORITIES - 1` (T), and one below it for everything else (P).

| Task | Core | Priority | Runs | Stack (estimate) | Blocks on |
|---|---|---|---|---|---|
| SDK flash lockout | 1 | T | only during a write (5.1) | 1 KB, static | nothing: spins in RAM with interrupts off |
| Logging and storage | 0 | T | when notified: records to write, a commit due, a file request | 1-2 KB | its notification |
| Flight | 0, alone | P | every 20 ms, woken by a hardware-timer alarm | 2-4 KB | its period only |
| Lua | 1 | P | whenever it is ready, sharing core1 with the two below | 12 KB (today's core1 stack) | nothing it did not ask for |
| Network: TinyUSB, lwIP, HTTP | 1 (section 7) | P | USB events, lwIP timers | 3-4 KB | TinyUSB's event queue, with a timeout for lwIP's timers |
| FreeRTOS timer daemon | 1 | P | the SDK's sync interop from interrupts | 2 KB | its queue |
| Idle, one per core | each | 0 | otherwise | 1 KB each | never |

`configUSE_TIME_SLICING` 1 shares core1 among Lua, the network and the
daemon a tick at a time when more than one is ready. Core0 holds only
the flight task at P, so slicing never takes core0 from it.

**The flight task's period comes from the hardware timer, not the tick.**
A write keeps interrupts off on both cores for its duration. The SysTick
counter wraps meanwhile, but at most one tick interrupt stays pending, so
a 3 ms write costs the kernel's tick count two ticks. `xTaskDelayUntil()`
would drift by those. Today the period is a deadline on the hardware timer
(`make_timeout_time_us()` in `main_hardware.c`), and so it stays: the
flight task waits on a notification from a 20 ms alarm on the hardware
timer, whose handler runs from RAM and schedules the next alarm from the
timer, not from the tick. A write delays one wakeup, never the ones after
it, and `hal_time_ms()` stays the flight's clock.

**Only the logging task writes flash.** Every other flash write becomes a
request to it: config and pins saves, uploads, OTA, the pad marker, and
any file a script writes. The requests travel on a queue, and the replies
come back as notifications. The rule the flash window keeps today,
"nothing writes flash while anything could be fetching from it", becomes
"only one task can write, and it locks the other core out first".

---

## 4. Scheduling two cores

FreeRTOS-Kernel V11 schedules the two RP2040 cores from one ready list.
Its `configRUN_MULTIPLE_PRIORITIES` setting decides whether tasks of
different priorities may run at the same time. **The plan uses 0, the
default, at your direction.**

### 4.1 What 0 does

- **When a task becomes ready, every running task of lower priority, on
  either core, yields** (`tasks.c`, `prvYieldForTask()`).
- **A core may then run only tasks of the highest ready priority, or its
  idle task:** "We can't schedule any tasks, other than idle, that have a
  priority lower than the priority of a task currently running on another
  core" (`tasks.c`, `prvSelectHighestPriorityTask()`).
- **Tasks of equal priority do not preempt one another.** One that becomes
  ready waits for a core to fall free: a task blocking, or, with time
  slicing, the next tick.

So with the flight task and Lua at P, pinned to cores 0 and 1, both run at
once. When the logger (T) wakes, both yield, and core1 drops to its idle
task until the logger blocks again. That is the behaviour you asked for.
Each wake costs a context switch on each core and one inter-core
interrupt, tens of microseconds (an estimate).

### 4.2 The rules it brings

1. **The logger must be at T, the SDK helper's priority.**
   `flash_safe_execute()` creates its helper on core1 at priority 0, then
   raises it to `configMAX_PRIORITIES - 1` (`pico_flash/flash.c`). On SMP,
   raising a ready task's priority calls `prvYieldForTask()` (`tasks.c`,
   `vTaskPrioritySet()`), which with 0 yields every running task below it,
   on both cores, the calling logger included. Then:
   - Core0 may run only its idle task: the helper is ready at T and is
     pinned to core1.
   - The helper disables core1's interrupts and spins until the logger
     releases it. That wait has no timeout.
   - Nothing runs again until the watchdog resets the board. In flight a
     watchdog reset comes back cold, and a cold board never deploys (L1).

   With the logger at T as well, logger and helper run together, one on
   each core, as intended. A build-time assertion holds it:
   `_Static_assert(LOGGER_PRIO == configMAX_PRIORITIES - 1)`.
2. **Everything else is at P.** A task below P starves whenever a script
   is running: Lua is at P and always ready while it computes, so core1
   has a task at P and core0 may run only the flight task or its idle
   task. A task above P but below T stops the flight task and Lua every
   time it runs. So the network task and the timer daemon join Lua at P.
3. **The flight task runs alone at P on core0.** Equal priorities do not
   preempt one another, so a task at P that could take core0 would
   make the flight task wait for it when it wakes. The network task and
   the daemon are therefore pinned to core1 with Lua. Core0 is idle
   whenever the flight task waits, roughly four fifths of each period,
   where today it spends that time on USB, lwIP and HTTP.
4. **Stopping both is not a lock.** A task is preempted wherever it is,
   possibly halfway through writing a record, so the handover stays a
   ring. What stopping both buys is quiet: the logger never runs beside
   the flight task or a script, so their timing is disturbed in one
   place, and the bus and the XIP cache are the logger's while it works.
   It does not make the flash safe on its own either: core1's idle task
   and its interrupt handlers run from flash, so the write still needs
   the lockout (section 5).

### 4.3 The alternative: 1

With `configRUN_MULTIPLE_PRIORITIES` 1, a core runs its own
highest-priority ready task whatever runs on the other. The logger then
preempts the flight task on core0 while Lua carries on on core1, and only
the lockout stops core1, for the write itself.

| | 0, your direction | 1 |
|---|---|---|
| While the logger works | the flight task and Lua both stop | the flight task stops; Lua runs on |
| During the flash write | both stopped, core1 locked out | both stopped, core1 locked out |
| The logger's priority | must be T, or the SDK's helper deadlocks | any above the flight task |
| The network task | at P on core1, sharing it with Lua | below the flight task on core0, in its slack, as today |
| Core0 while the flight task waits | idle | USB, lwIP and HTTP |
| HTTP throughput while a script runs | about half of core1 (time slicing) | core0's slack, as today |
| On the ESP32-S3 | IDF schedules each core on its own, as 1 does (section 11), so the logger stops only its own core there | the same as IDF |

`vTaskSuspendAll()` is not a way to block the system for a write, in
either mode. On SMP it stops task switching, but the task already running
on the other core keeps running, from flash. The SDK says the same of
`taskENTER_CRITICAL`: "on SMP it only prevents the other core from also
entering a critical section" (`pico_flash/flash.c`, SDK 2.2.0).

---

## 5. The write

### 5.1 The lockout the SDK provides

`flash_safe_execute(func, param, timeout_ms)` (`pico_flash`, SDK 2.2.0),
built with `PICO_FLASH_SAFE_EXECUTE_SUPPORT_FREERTOS_SMP`, does this:

1. It creates a task on the other core at `configMAX_PRIORITIES - 1`,
   running `flash_lockout_task`, which is `__not_in_flash_func`. It
   creates the task afresh on every call, from a single static stack when
   dynamic allocation is off.
2. It waits, up to `timeout_ms`, for that task to disable interrupts on
   its core and report ready. It fails with `PICO_ERROR_TIMEOUT` if not.
3. It disables interrupts on its own core, runs `func` (for us,
   `flash_range_program()` of one page), and restores them.
4. It releases the other core and waits, up to `timeout_ms`, for it to
   report done.

Both cores are therefore out of execute-in-place, with interrupts off,
for the length of the program plus two handshakes. Nothing else runs:
not the flight task, not Lua, not the USB interrupt, not the MS5607's
alarm. Every wait in it is bounded by `timeout_ms`. A core1 that never
answers, because it is stuck with interrupts off or has faulted, costs a
failed write, never a hang.

Two refinements, both small:

- **A persistent lockout helper.** The SDK looks the helper up through a
  weak `get_flash_safety_helper()`. Ours would keep one lockout task at T,
  created at boot and pinned to core1, blocked on a notification, instead
  of creating one per page and raising its priority. That takes the
  kernel's task creation out of every write, and with it the priority
  raise that 4.2 rule 1 guards against.
- **Nothing preempts the logger in the gap.** The SDK notes that the
  caller "may get preempted" after core1 is parked and before it disables
  its own interrupts. Nothing ranks above the logger, so only an
  interrupt can run there, and it lengthens core1's wait by its own
  duration.

### 5.2 How long the system stops

| Part | Board | Page program, 256 B (typ / max) | 4 KB sector erase (typ / max) | 64 KB block erase (typ / max) | Source |
|---|---|---|---|---|---|
| W25Q128JV | MK1A | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | `W25Q128JV_RevH_2021-03-10.pdf`, page 66 |
| W25Q16JV | MK1B as built | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | `W25Q16JV_RevI_2024-12-24.pdf`, page 64 |
| BY25Q64ES | a possible MK1B replacement | 0.45-0.6 / 2.4 ms | 35-50 / 300-400 ms | 0.18 / 2 s | `BY25Q64ES_Rev2.9_2024-10-29.pdf`, pages 71-75 |
| XT25F128F | MK1C | to be read | to be read | to be read | its datasheet is not yet in `docs/datasheets/` |

`flash_range_program()` adds leaving and re-entering execute-in-place, and
flushes the XIP cache. The flush takes "just over 1024 clock cycles"
(`rp2040-datasheet_2025-02-20.pdf`, page 124), about 8 us at 125 MHz. Both
cores then refill the 16 KB cache (page 123) from flash as they run, which
slows the first microseconds after every write (an estimate; to be
measured).

**So a page write stops the system for about 0.5 ms typically and 3 ms at
worst, and an erase for 45 ms typically and 400 ms at worst.** Pages
belong in flight. Erases belong on the pad, before OK to LAUNCH.

### 5.3 The log as pre-erased pages (RAW-INT)

Plan 1 describes RAW-INT: a raw region of the code flash, erased on the pad
and built into a littlefs file after landing (`log_storage_options.md`,
section 3.3). Plan 2 depends on it:

- **Pre-erase on the pad.** Before OK to LAUNCH sounds (C6), the logging
  task erases the region one 4 KB sector at a time, each a separate
  lockout, with the flight task running between them. Each is a
  whole-system stop of 45 ms typically and 400 ms at worst. The watchdog
  (1000 ms) is fed between them. A 1 MB region is 256 sectors: about 12 s
  typically, and more than a minute and a half at worst. The time is spent
  once, on the pad, while the board is not yet OK to fly.
- **In flight, only page programs.** The ring fills a page buffer. A full
  page is programmed in one lockout.
- **The one-second commit (FLT-LOG-06) without an erase.** Each second
  the logger programs whatever the current page has gained since the last
  commit, into bytes still erased; nothing is rewritten. The W25Q16JV
  allows it: a partial page "can be programmed without having any effect
  on other bytes within the same page", into erased (FFh) locations
  (`W25Q16JV_RevI_2024-12-24.pdf`, page 34). It states no limit on how
  many times one page may be partly programmed; that is to be confirmed
  for each part.
- **After landing,** the region's records are copied into littlefs as
  `flight_log.bin`, with the erases that takes, now harmless. The download
  path stays as it is (DD-062).
- **A reset in flight** finds the region's end by scanning for the first
  erased record, and appends from there (L3).

How often the system stops, from `FLOG_SAMPLE_BYTES` (22) in
`src/flight_log.h` and the plans in DD-064:

| Logging plan | Bytes a second | Page programs a second | Commits a second | Stopped, typical | Stopped, worst |
|---|---|---|---|---|---|
| A row a second (default) | 22 | one each 11.6 s | 1 | 0.5 ms/s | 3 ms/s |
| High rate 2, every sample, 50 a second | 1,100 | 4.3 | 1 | 2.7 ms/s | 16 ms/s |
| High rate 1 | between them | | | | |

Against the 20 ms period, a single stop of at most 3 ms, placed after the
flight task's step, costs no deadline.

### 5.4 Placing a write in the period

A write stops the MS5607's alarm and disturbs a conversion running beside
it. DD-068 already discards a conversion that a flash operation overlapped.
At High rate 2 that is up to 5 conversions of 50 a second (the 4.3
programs and the commit). Two ways to handle it; the choice is yours
(section 13):

- **Accept the discards.** It is what DD-068 does for today's writes, and
  the pressure fit tolerates missing samples (T11).
- **Write in the gap.** An MS5607 board converts a pressure and a
  temperature each period, 8.22 ms each typically and 9.04 ms at most at
  OSR 4096 (`MS5607-02BA03_2017-06.pdf`, page 3), which leaves about
  3.5 ms a period idle typically, and 1.9 ms at worst. MK1A's BMP280
  converts once a period, 13.3 ms at most (`BMP280_MEAS_MAX_US`). The
  flight task knows the schedule, and wakes the
  logger at the start of the idle stretch. A typical page program, 0.4 ms,
  fits. A worst-case one, 3 ms, may not, and is counted when it overlaps.

### 5.5 What else stops

| Stopped for up to 3 ms | Effect | Answer |
|---|---|---|
| USB | The host retries a NAKed transfer; the start-of-frame counter keeps counting in hardware, so `usb_host_active()` is unaffected | none needed |
| UART receive | The RX FIFO holds 32 characters (`rp2040-datasheet_2025-02-20.pdf`, page 418), 2.8 ms at 115,200 baud | Ground test commands are read only in PAD_IDLE, when no page is written. A sector erase on the pad (5.3) can drop them; the pre-erase runs before OK to LAUNCH, so the serial ground test waits for it |
| MK1C's arm pump | PIO keeps pumping without the CPU. The loop-stepped firing sequence (DD-056) and the precharge timeout (L6, about 35 ms) are paced by the flight task | The logger asks `board_flash_ok()`, as the window does today, and writes nothing during a fire's sequence |
| Lua | Paused wherever it is | none needed |

---

## 6. The flight task

- **One step a period:** the same calls as stages 2-6 today
  (`hal_tasks_tick()`, `dispatch_state()`, `flight_update_outputs()`,
  `update_status()`), then the notification to the logger, then the wait.
- **Records go into a ring.** `hal_log_sample()` writes a record into a
  single-producer, single-consumer ring in RAM and calls
  `xTaskNotifyGive()` on the logger. Neither call waits. A full ring drops
  the record and counts it (`log_dropped`, as today).
- **Nothing on its path takes a lock:** no `printf`, no `malloc`, no
  littlefs. Today's code already keeps `malloc` off it (`LFS_NO_MALLOC`,
  `docs/core1_hazard.md`). Status for the web interface goes out through a
  seqlock, as `lua_core1_publish()` does today.
- **The watchdog.** Fed by the flight task, and only when the logger has
  also checked in within its own budget, so a stuck logger resets the board
  on the ground. In flight, a logger that has stopped is counted and the
  flight carries on (L1). Lua and the network do not hold the watchdog.

---

## 7. Lua and the network

**Lua is a task pinned to core1.** Its interpreter, its 32 KB arena and
its 12 KB stack are today's. What goes:

- **The unit dispatch, the grant budgets and the RAM idle loop.** They
  exist so that core0 knows core1 is out of flash before it writes
  (`lua_core1.h`). The lockout replaces all three.
- **`lua_core1_flash_ok()`.** The flash is safe whenever the logger
  holds the lockout.
- **The core1 kill.** Under SMP FreeRTOS, core1 runs the scheduler, the
  timer daemon and the lockout helper as well as Lua. Stopping it with
  `frce_off` could leave one of the kernel's two spin locks
  (`configSMP_SPINLOCK_0/1`, `rp2040_config.h`) held, and core0's next
  kernel call would wait forever. A script that runs away needs no kill:
  it is preempted like any task, and core0 does not depend on it.
  Lua's instruction hook can still end a script that exceeds a budget.
- **The FIFO launch handshake.** The kernel starts core1 itself, with the
  SDK's `multicore_reset_core1()` and `multicore_launch_core1()`, and
  uses the inter-core FIFO's interrupt to make the other core reschedule
  (`portable/ThirdParty/GCC/RP2040/port.c`). Nothing of ours may use the
  FIFO. The SDK's `multicore_lockout` also uses it, which is why
  `flash_safe_execute()` has a FreeRTOS path of its own.

**A core1 that faults.** Core0 runs on, but every lockout times out
(5.1), so the log stops being written. Mitigation: a core1 HardFault
handler in RAM that parks the core and sets a flag. The logger then
knows core1 is out of flash, and writes with only its own interrupts off.

**The network task.** TinyUSB with its FreeRTOS abstraction
(`CFG_TUSB_OS OPT_OS_FREERTOS`), lwIP kept as `NO_SYS` inside this one
task, and the HTTP server. HTTP's work splits into units today only
because core0 had to share its slack with them; as a task it needs no
units. The rule about TinyUSB callbacks (never start an endpoint transfer
from one) stays. The USB interrupt stays on core0 and hands its events to
the task through TinyUSB's queue.

**Where it runs.** Under 4.2 it shares core1 and priority P with Lua, a
tick at a time when both are ready. A busy script halves HTTP's share of
core1; an idle one leaves it all. Two other placements, each with a cost:

- **Free to use either core, at P.** It fills core0's idle time, but a
  flight task that wakes to find it on core0 waits for it to block, or
  for the next tick with time slicing: up to 1 ms of jitter on the
  period.
- **Under 4.3's mode 1 instead,** on core0 below the flight task, as
  today.

**A runaway script** no longer threatens the flight task: it shares
core1 with the network task by time slicing, and the flight task has
core0 to itself.

---

## 8. The safety argument

Today's rule is "core0 must never wait on anything core1 can hold"
(`docs/core1_hazard.md`). Under plan 2 it becomes:

> **The flight task never waits on anything another task can hold. Its
> only blocking call is the wait for its next period.**

What discharges it:

- **The handover never blocks:** a ring write and `xTaskNotifyGive()`.
- **No lock on the flight path:** no mutex, semaphore, queue receive,
  `printf` or `malloc`.
- **The kernel's own locks are bounded.** Every kernel call, on either
  core, enters a critical section that takes the kernel's spin locks with
  interrupts off. So the flight task's `xTaskNotifyGive()` can wait on
  core1, but only for as long as the longest kernel critical section. That
  is kernel code, bounded by the number of tasks, not user code. This is
  the one wait on the other core that plan 2 adds, and the proof has to
  state it rather than hide it.
- **The write's waits are bounded** by `flash_safe_execute()`'s timeout
  (5.1), provided the logger is at T (4.2, rule 1). The program itself is
  bounded by the part's maximum.
- **The priorities are checked at build time:** the logger and the
  lockout helper at T, every other task at P, and the flight task the
  only one pinned to core0 at P.

**`prove_core0.py` changes** from "no flight root calls an unbounded
primitive" to three checks:

- **No blocking-capable kernel call from a flight root.** The call graph
  cannot see a timeout argument, so every blocking-capable call is
  refused by name (`xQueueReceive`, `xSemaphoreTake`, `ulTaskNotifyTake`,
  `xEventGroupWaitBits`, `vTaskDelay`, ...). One exception is allowed: the
  period wait, in the task's top-level loop.
- **The kernel's critical-section entry is reported as a bounded,
  other-core wait**, the way hardware spin locks are reported today.
- **One boot path is accepted:** `vTaskStartScheduler()` reaches
  `multicore_reset_core1()`, which ends in `multicore_fifo_pop_blocking()`.
  That runs once, against a cold core1, before the watchdog is armed.
  Today's `lua_core1_start()` makes the same argument for its own launch.

**`wait_check.py` changes** likewise. DD-053's "the exec loop is the only
clock" becomes: the flight task's period is the only clock on the flight
path, and blocking waits are allowed in the other tasks, each with a
timeout.

**Stack overflow** is checked by the kernel (`configCHECK_FOR_STACK_OVERFLOW`
2), and the stacks are sized from a measurement: core0's stack high-water
mark is not measured today (section 12, step 1).

---

## 9. Memory

Measured on 2026-09-28 by building two small RP2040 images that differ
only in FreeRTOS. Each has four tasks, a queue and a
`flash_safe_execute()` page write. Built with SDK 2.2.0, GCC 14.2 Rel1,
Release, and FreeRTOS-Kernel V11.1.0+ at commit 8be86d4 (2026-08-26), using
static allocation, core affinity, timers on (the SDK interop needs them),
and trace facilities off. The figures are symbol sizes in the linked image,
by source file:

| Component | Code and constants | RAM |
|---|---|---|
| `tasks.c` | 10,520 B | 4,702 B (kernel state; the two idle stacks and the timer task's stack) |
| `queue.c` | 2,190 B | 0 |
| `timers.c` | 1,138 B | 260 B |
| `port.c` (RP2040 SMP port) | 1,117 B | 36 B |
| `event_groups.c` | 522 B | 0 |
| `list.c` | 128 B | 0 |
| **FreeRTOS in total** | **15,615 B** | **4,998 B** |
| SDK `pico_flash` with the SMP lockout | 512 B | 1,130 B (the lockout's static stack) |

Against today's images (2.1.702, `arm-none-eabi-size`):

| Board | Code and data today | Slot | Free | Free after FreeRTOS |
|---|---|---|---|---|
| MK1A | 362,748 B | 3,948 KB | plenty | plenty |
| MK1B | 358,908 B | 384 KB (393,216 B) | 34,308 B | about 18 KB, before the code plan 2 removes |
| MK1C | 366,844 B | 3,948 KB | plenty | plenty |

**RAM.** Today core0 runs on the SDK's main stack (`PICO_STACK_SIZE`,
2 KB reserved), and core1 on a 12 KB array. Under plan 2 the main stack
serves interrupts; the Lua task keeps its 12 KB; and the flight, logger and
network tasks need stacks of their own, about 6-10 KB together (an
estimate, until core0's use is measured). With the kernel's 5 KB and the
lockout's 1 KB, plan 2 needs roughly 12-16 KB of SRAM. R1, for comparison,
needs about 40 KB for code and constants in RAM, of the 50.6 KB free
(`docs/outstanding_tasks.md`, R1).

---

## 10. Plan 1 and plan 2 side by side

| | Plan 1: R1 | Plan 2: FreeRTOS SMP |
|---|---|---|
| The flight code during a write | runs, from RAM | pauses: about 0.5 ms typically, 3 ms at most, per page |
| Sensor samples during a write | the alarm keeps running; the overlapped conversion is still discarded (DD-068) | the alarm is late by the write; the same discard |
| What makes flash safe | core1 does the write as a unit, while core0 runs only RAM code; an MPU guard; the proof of a RAM-closed flight path | the SDK's lockout; one writer |
| The log in flight | any medium; RAW-INT on today's boards | RAW-INT on today's boards (erases must stay out of flight) |
| SRAM | about 40 KB of code and constants; 7-10 KB left | about 12-16 KB of kernel and stacks |
| MK1B flash | unchanged | about 16 KB more code |
| Lua | units on core1, as today | a task at the flight task's priority, sharing core1 with the network; units, grants and the kill removed |
| The proof | the flight closure RAM-closed, and no unbounded wait | no blocking kernel call from the flight task; the kernel's spin locks named as a bounded wait |
| Toward the ESP32-S3 | the RAM flight path maps to IRAM, but the core1-writer does not: ESP-IDF parks the other core during a write | the same model ESP-IDF uses |
| Effort (rough estimate) | 18-27 engineer-days with the common layer | 21-33 (section 12) |

The two combine with plan 1's media. On an SD card or another NONE-class
medium the logger needs no lockout at all; on the RP2350's second flash
(BRIEF) it needs one for microseconds. Plan 2 is an answer for FULL-class
media, the code flash, on today's boards.

---

## 11. The move to the ESP32-S3

What carries over:

- **The task graph, ring and notifications.** One difference in the
  scheduling: IDF FreeRTOS schedules each core independently, and "the
  core will select the highest priority ready-state task that can be run
  by the core" (ESP-IDF v6.1, "FreeRTOS (IDF)"). That is 4.3's mode 1. On
  the ESP32-S3 the logger stops only its own core, and Lua runs on
  through its work. Nothing in the plan depends on the logger excluding
  Lua (4.2, rule 4), so the tasks carry over; the network task can move
  back beside the flight task, on core0 below it.
- **The fully blocking write is ESP-IDF's own.** During a write to the
  flash on SPI0/1, "all other tasks are suspended. The other core will be
  polling in a busy loop", and only IRAM-safe interrupts run (ESP-IDF v6.1,
  "Concurrency Constraints for Flash on SPI0/1"). `esp_partition_write()`
  does the lockout itself, so the logger's write shrinks to that call.
- **Better than the RP2040:** an interrupt registered with
  `ESP_INTR_FLAG_IRAM`, with its code and data in IRAM and DRAM, keeps
  running through the write. The MS5607's alarm can keep sampling.
- **Auto-suspend,** if the flash chip supports it. ESP-IDF can suspend an
  erase to let the caches run. It needs the SUS bit at SR2 bit 7 and 75h
  and 7Ah as suspend and resume ("Concurrency Constraints for Flash on
  SPI0/1"). The W25Q16JV and the BY25Q64ES both qualify (W25Q16JV, pages
  15 and 40; BY25Q64ES, page 17 and section 7.4.7). With it, even an erase
  would not park the system.
- **RAW-INT maps to a raw partition, and littlefs to the `esp_littlefs`
  component.**

What changes:

- **API names.** IDF uses `xTaskCreatePinnedToCore()` and gives stack
  sizes in bytes, not words; V11 SMP uses `xTaskCreateAffinitySet()` and
  `vTaskCoreAffinitySet()`. IDF FreeRTOS is "based on Vanilla FreeRTOS
  v10.5.1 but contains significant modifications" ("FreeRTOS (IDF)"). A thin
  header of our own, wrapping task creation, pinning, notification and the
  flash write, keeps the tasks' code identical on both.
- **Everything below `hal.h`:** the drivers, the arm pump (no PIO on the
  ESP32-S3), USB through ESP-IDF's TinyUSB, OTA through its partitions
  (plan 1, option 2).

Plan 1 estimates the whole port at 40-80 engineer-days. With plan 2 done
first, the tasks, the Lua task and the logger exist already; a rough guess
is that it saves 5-10 of them.

---

## 12. Work, in order, tests first

Estimates are rough, for comparison only.

1. **Measure first (1-2 days).** Paint core0's stack and report its
   high-water mark; record the loop's stage times. These size the tasks.
2. **FreeRTOS in the build, the superloop as one task (2-3 days).** The
   kernel, the config above, static allocation, the SDK interop. Tests:
   every host suite unchanged (the flight logic stays tick-driven); G4 on
   all boards with the same results.
3. **The flight task and the logger (4-6 days).** The ring, the
   notification, the alarm-driven period, the persistent lockout helper,
   and the watchdog check-ins. Tests first: the ring drops and counts when
   full; the flight task makes no blocking call (the new
   `prove_core0.py` rule fails on today's image, then passes); a logger that
   stops is counted in flight and resets the board on the ground.
4. **RAW-INT (5-8 days; plan 1's common layer, shared).** Pre-erase before
   OK to LAUNCH, page programs, the one-second commit without an erase,
   the scan-and-append after a reset, the copy into littlefs after landing,
   two flights kept (C6). Tests first: the host flash model refuses an
   erase in any flight state; a reset in flight resumes the same log (L3);
   a half-written record is counted (L4).
5. **Lua and the network as tasks (5-8 days).** Remove the units, grants,
   RAM idle loop, kill and FIFO handshake; TinyUSB's FreeRTOS abstraction.
   Tests: `lua_tests` and `http_tests` unchanged; a runaway script leaves
   the flight task's period intact on the bench.
6. **The proof, the wait rules and the priorities (2-3 days).** Tests
   first: the build fails with the logger below T, with a task between P
   and T, or with a second task at P on core0 (4.2).
7. **Bench (2-3 days).** On each board, with Lua running and High rate 2:
   loop jitter and overruns, a histogram of lockout durations, the
   conversions discarded, and G4.

**Total: about 21-33 engineer-days**, against plan 1's 18-27 for today's
boards. Steps 1, 4 and 7 are needed by either plan.

---

## 13. Decisions for you

1. **Plan 1 or plan 2 for today's boards.** They are alternatives for the
   code flash. Plan 2 costs a pause of up to 3 ms per page in flight;
   plan 1 costs 40 KB of SRAM and a RAM-closed flight path to prove.
2. **The raw log region's size on each board,** taken from littlefs or the
   OTA slots. It was open for MK1B already (C6); on 2 MB there is little
   room.
3. **Discards or the gap (5.4):** accept a discarded conversion per write,
   or schedule writes into the sensor's idle stretch.
4. **The scheduling mode:** 0, as you directed, with the network sharing
   core1 with Lua; or 1, with the network in core0's slack as today
   (4.3). Under 0, whether the network may also use core0, at the cost
   of up to 1 ms of jitter on the flight task's period.
5. **A runaway script:** preempted and left running, or ended by an
   instruction budget.

---

## 14. Risks

- **A 3 ms stop is the part's maximum, not a measurement.** The histogram
  of step 7 decides whether the typical 0.4 ms holds on these boards.
- **The SDK helper's deadlock under mode 0** if the logger is ever below
  T (4.2, rule 1). The build-time assertion, and a persistent helper of
  our own that never raises its priority, both guard it.
- **Kernel spin locks are a wait on the other core.** They are bounded,
  but they break today's rule's letter, and the proof must say so.
- **XIP cache contention** between the two cores' code, and the cache
  flush after every write, add jitter that today's design also has.
- **The kernel's own maturity on this port.** Tickless idle is untested
  (the port's `README.md`, "Known Limitations"); plan 2 does not use it.
  SMP FreeRTOS is newer than single-core FreeRTOS.
- **MK1B's slot** is left with about 18 KB after FreeRTOS, less whatever
  plan 2 removes. Lua is already built for size there.
- **Prior art:** `~/Documents/blink` is a FreeRTOS skeleton on the same
  port, with FatFs on an SD card. It sets `configRUN_MULTIPLE_PRIORITIES` 1,
  as section 4 does. It fetches V11.1.0 but sets `configNUM_CORES` 2, the
  name before V11; V11.1.0 reads only `configNUMBER_OF_CORES` and defaults
  it to 1 (`include/FreeRTOS.h`), so as written it runs on one core.

---

## 15. Sources

- In `docs/datasheets/`:
  - `W25Q16JV_RevI_2024-12-24.pdf`, pages 15 and 40 (suspend and resume),
    34 (partial page programs) and 64 (AC characteristics).
  - `W25Q128JV_RevH_2021-03-10.pdf`, page 66.
  - `BY25Q64ES_Rev2.9_2024-10-29.pdf`, pages 17 and 71-75, section 7.4.7.
  - `MS5607-02BA03_2017-06.pdf`, page 3.
  - `rp2040-datasheet_2025-02-20.pdf`, pages 123-124 (XIP cache) and 418
    (UART FIFOs).
- Raspberry Pi pico-sdk 2.2.0: `src/rp2_common/pico_flash/flash.c`
  (`flash_safe_execute()` and its FreeRTOS SMP lockout);
  `src/rp2040/pico_platform/include/pico/platform.h` (`PICO_STACK_SIZE`).
- FreeRTOS-Kernel, github.com/FreeRTOS/FreeRTOS-Kernel, commit 8be86d4
  (2026-08-26), V11.1.0+: `include/FreeRTOS.h`, `tasks.c`
  (`prvYieldForTask()`), `portable/ThirdParty/GCC/RP2040/port.c`,
  `include/rp2040_config.h` and `README.md`.
- ESP-IDF Programming Guide v6.1, ESP32-S3: "FreeRTOS (IDF)" and
  "Concurrency Constraints for Flash on SPI0/1", docs.espressif.com,
  fetched 2026-09-28.
- In this tree: `docs/log_storage_options.md` (plan 1);
  `docs/outstanding_tasks.md` section 10 (L1-L6, R1, C6);
  `docs/core1_hazard.md`; `src/main_hardware.c`, `src/flash_window.c`,
  `src/littlefs_driver.c`, `src/hal_common/hal_common.c`,
  `src/lua/lua_core1.h`, `src/flight_log.h`; DD-053, DD-056, DD-062,
  DD-064, DD-065, DD-066, DD-068.
- To add before building: MK1C's XT25F128F datasheet.
