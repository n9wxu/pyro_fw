# Lua User Programs on Core1 — Implementation Plan

## Overview

Run user-supplied Lua programs on the RP2040's second core, so a flyer can
drive night-launch LEDs, feed a radio telemetry link, log custom data, or test
a flight algorithm, without touching the pyro application.

The whole design rests on one rule: **the pyro application has priority and
must never wait for Lua.** Everything below follows from that.

Scope is MK1C and later. See *Key discoveries* for why MK1B is excluded.

## Current State Analysis

Measured on the current tree, not assumed:

| | |
|---|---|
| RAM in use (MK1C) | 70 KB of 264 KB — ~194 KB headroom |
| Core1 | completely unused, no `multicore` calls anywhere |
| App slot, MK1C | 3948 KB, holding 182 KB |
| App slot, MK1B | 384 KB, holding 173 KB |
| Flash writes in flight | yes — `log_task` flushes the RAM ring asynchronously, plus a synchronous flush path when a line does not fit |

### Key Discoveries

- **Lua does not fit on MK1B.** The Lua 5.4 core is roughly 150–200 KB of
  flash. MK1B's A/B slot is 384 KB and already holds 173 KB. MK1C's slot is
  3948 KB. This becomes a board capability (`BOARD_HAS_LUA`), which the
  `boards/<name>/` structure already supports.

- **Lua's pins are J3 plus J1.6.** J3 is a user breakout present on all MK1C
  hardware: castellated pads carrying GPIO18–21 with 3.3 V and GND, meant to be
  soldered straight into the flyer's av-bay. With GPIO22 on J1.6 that is five
  pins. GPIO2,3,4,5,9,10,13,14,15 have no pad at all, and GPIO0/1 stay with
  telemetry on J1.4/J1.5.

- **The allowlist must cover pin FUNCTION, not just pin number.** This is the
  sharpest trap in the whole design:

  | pin | I2C function | UART function |
  |---|---|---|
  | GPIO18 | **I2C1 SDA** | — |
  | GPIO19 | **I2C1 SCL** | — |
  | GPIO20 | I2C0 SDA | UART1 TX |
  | GPIO21 | I2C0 SCL | UART1 RX |

  `i2c1` is the pressure sensor bus. `gpio_set_function(18, GPIO_FUNC_I2C)`
  would put GPIO18/19 on the *same peripheral instance* as the MS5607 on
  GPIO6/7, electrically joining them and corrupting the primary bus — using
  only pins that are legitimately on Lua's allowlist. A pin-number allowlist
  does not catch this. See invariant L3.

- **Core1 cannot execute while core0 writes flash.** XIP must be disabled for
  `flash_range_erase`/`flash_range_program`, and Lua's interpreter is far too
  large to run from RAM. **Decision: multicore lockout**, accepting that Lua
  stalls up to ~45 ms on a sector erase. This is the single largest behavioural
  caveat and every timing-sensitive Lua API must tolerate it.

- **Neither lockout variant is usable.** `multicore_lockout_start_blocking()`
  hangs core0 forever on a wedged core1; `_timeout_us()` still blocks core0 for
  the timeout. Both let core1 delay pyro. Core0 must poll and defer instead,
  with PSM force-off as the unilateral fallback — see invariant L3.

- **Core1 can read flash but not write it.** littlefs reads are memory-mapped
  through XIP and are safe from either core; writes are not. But `lfs_t` is not
  concurrency-safe, so sharing a mount across cores is a data race regardless.
  **Core0 owns the filesystem outright**; core1 never calls littlefs.

- **RP2040 has no MPU.** The sandbox is therefore API-level, not hardware
  enforced. A Lua script cannot reach memory the C bindings do not hand it, but
  a bug in a binding is an escape. The bindings are the security boundary and
  should be reviewed as such.

- **mbedtls is already linked** for `pico_fota_bootloader`, so SHA1 and base64
  for the WebSocket handshake need no new dependency.

## Desired End State

A flyer can upload several `.lua` files, assign them to flight events from a
configuration tab, have one run continuously, and interact with the VM from a
browser console — while the flight computer behaves exactly as it does today.

**Verification**: a soak test where Lua deliberately misbehaves (infinite loop,
OOM, wild pin writes, filesystem hammering) and a full simulated flight runs to
completion with correct pyro timing and an intact flight log.

## What "must not interfere" actually means

Delay to the firing code is not the risk. At apogee the vehicle is nearly
stationary, so even a pathological stall costs centimetres:

| stall | duration | at apogee | under drogue (20 m/s) | ballistic (111 m/s) |
|---|---|---|---|---|
| flash page program | 0.7 ms | 0.0 cm | 0.05 ft | 0.3 ft |
| page program, max spec | 3 ms | 0.0 cm | 0.20 ft | 1.1 ft |
| sector erase, typical | 45 ms | 1.0 cm | 2.95 ft | 16.4 ft |
| sector erase, worst spec | 400 ms | 78 cm | 26 ft | 146 ft |

Those stalls are **pre-existing and not caused by Lua** — core0 already erases a
sector roughly once a second while logging at 100 Hz, and has always done so.
Lua adds essentially nothing to them, because core0 never waits for core1.

The risk is **not firing at all, or freezing part-way through a fire.** Every
invariant below exists for that, not for jitter. The enumerated ways it could
happen, and what structurally prevents each:

| failure | mechanism | prevented by |
|---|---|---|
| core0 hangs on a lock a dead core1 holds | `mutex_enter_blocking()` never times out | L2 + the CI `nm` gate |
| core0 waits for core1 to park, forever | blocking lockout variants | L3 poll-and-defer, PSM force-off |
| XIP left disabled by a crashed writer | core0 cannot fetch instructions | core0 is the only flash writer |
| core1 corrupts core0 state | no MPU on RP2040 | fixed arena, bounds-checked allocator — **accepted risk, not eliminated** |
| watchdog resets mid-fire | armed-window watchdog not fed during F7/F8 | see the note in the MK1C plan |

The last row is not hypothetical and is tracked separately: the arm-window
watchdog added for the pump must have a timeout exceeding the *whole* F0–F10
sequence, because the pump stops at F6 and nothing feeds it through the misfire
hold in F7. A reset there would both fail to deploy and open the gate with
current still flowing.

## Safety Invariants

These are to Lua what `DESIGN.md` §9 is to the pyro circuit. Numbered for
reference in code comments.

**L1. Core0 never blocks on core1.** Every handshake uses a timeout. On expiry
core0 resets core1 and continues.

**L2. Core1 never takes a shared lock and never allocates.** This — not the
length of a peripheral operation — is how core1 could block core0. A busy-wait
on core1 costs core1 only. Holding a lock core0 wants is what hangs core0, and
`mutex_enter_blocking()` has no timeout:

| forbidden on core1 | why |
|---|---|
| `malloc` / `free` / anything calling them | `malloc_mutex`, `mutex_enter_blocking()` spins forever |
| `dma_claim_*`, `pio_claim_*`, any `hardware_claim` | `PICO_SPINLOCK_ID_HARDWARE_CLAIM` |
| `add_alarm_*`, alarm pools, `critical_section_*` | `PICO_SPINLOCK_ID_TIMER`, striped locks |
| `queue_t`, `mutex_t`, `semaphore_t` | striped spinlocks |
| `printf` / stdio | allocates and locks |
| runtime `irq_set_*` | `PICO_SPINLOCK_ID_IRQ` |

Core1 may drive peripherals it exclusively owns, because that path takes no
lock: `gpio_put` is a SIO register write, and the UART/I2C/PWM transfer
functions only poll FIFO status. Core0 initialises and claims every resource
before launching core1; core1 only *uses* what is already claimed.

Enforced by an `nm` gate over the core1 translation units in CI, not by
convention.

**L3. Core1 can never block a flash write, and core0 never waits for it.**
Core1 executes from flash, so it must be out of flash before XIP is disabled.
Core0 obtains that by *polling*, never by waiting: if core1 is not already
parked, the write is deferred to a later main-loop iteration. A core1 that
never parks costs flight-log latency and nothing else. See *Parking* below.

Interrupts on core1 are permitted and are used for exactly this — an IRQ gets
core1 to a safe point promptly even if it is inside a long C call.

**L4. The Lua environment is built from configuration at startup.** A capability
that is not enabled is not merely refused — its table is absent from the
environment, so a script referencing it fails immediately and legibly rather
than discovering a permission error at the worst moment.

**L5. Nothing in the Lua API names hardware.** No pin numbers, no peripheral
instances, no SDK types or constants. Scripts address resources by the symbolic
names configuration gave them. A script therefore *cannot express* access to a
resource it was not granted.

**L6. Lua never writes flash.** All filesystem writes are requests to core0.

**L7. Lua's heap is a fixed arena.** Exhaustion fails the script, never core0.

**L8. Every Lua invocation has an instruction budget.** Overrun aborts the
script and logs it.

**L9. Core0 spends a bounded budget per main-loop iteration serving Lua.**
Overflow means Lua waits; it never means the flight loop waits. Bulk transfers
go to DMA so the wire time is not core0's problem.

**L10. Core1 failure never resets core0.** A hang, panic or OOM on core1 is
contained: core0 detects it by heartbeat and restarts core1.

**L11. Lua is read-only with respect to flight.** No API can arm, fire, disarm,
change a pyro mode, or alter the flight state machine.

## What We're NOT Doing

- Not giving Lua any write path to pyro, and not exposing a "fire" API behind a
  flag. `DESIGN.md` invariant 11 says every fire command names an authority and
  there is no third source; a user script would be exactly that third source.
- Not running Lua on MK1B — it does not fit.
- Not making Lua real-time. This is explicit, not a regret: flash activity
  parks core1 for tens of ms and core0 serves I/O on a bounded budget, so Lua
  timing is best-effort by design.
- Not letting core1 allocate, claim, or take any shared lock — see L2 for the
  enumerated list and the CI gate.
- Not using blocking peripheral calls on core1; timeout variants only, so a
  stuck slave cannot hold the VM indefinitely.
- Not letting core0 wait on core1 for any reason, including flash access.
- Not exposing the Pico SDK — no pin numbers, no instances, no SDK constants
  or types anywhere in the Lua-visible surface.
- Not sharing `lfs_t` across cores.
- Not implementing a Lua debugger, `require` of arbitrary modules, `os.execute`,
  or the `debug` library.
- Not letting Lua survive a reset with state. Each boot starts clean.

## Architecture

Core1 drives the peripherals configuration granted it, and nothing else. What
makes that safe is not avoiding peripherals — it is that core1 never touches a
shared lock, never allocates, and never claims a resource (L2). Core0 sets
everything up before core1 launches; core1 only uses it.

Core1 still cannot name hardware: the Lua API is symbolic, so a script asks for
"beacon", not GPIO18.

```
  CORE 0  (pyro application, priority)   CORE 1  (Lua, compute only)
  -----------------------------------    ----------------------------
  flight state machine                   Lua 5.4 VM
  initialises + claims ALL resources     fixed-arena allocator, no malloc
  littlefs (sole owner)                  instruction-budget hook
  owns flash, pressure, pyro, telemetry  drives ONLY its own peripherals
  never waits on core1                   no locks, no claims, no alloc
                                         timeout variants only

        |  state snapshot (seqlock, lock-free)  -->  |
        |  lock-free SPSC rings (never queue_t) <->  |
        |  park request (IRQ)                   -->  | parks in RAM; core0 POLLS
        |                                            | and defers, never waits
```

### Parking: core0 polls, never waits

This is the sharpest constraint in the design. Core1 runs from flash, so core0
cannot disable XIP while core1 is executing. Every obvious mechanism gets this
wrong by making core0 wait:

| approach | why it fails |
|---|---|
| `multicore_lockout_start_blocking()` | hangs core0 forever on a wedged core1 |
| `multicore_lockout_start_timeout_us()` | still blocks core0 for the timeout |
| Lua entirely in RAM | ~150 KB of Lua against ~194 KB free — no margin |

The protocol therefore never blocks:

1. `multicore_lockout_victim_init()` on core1, so an IRQ parks it promptly in a
   RAM routine even mid-C-call. Only that routine and its spin need to be
   `__not_in_flash_func`; the rest of Lua stays in flash.
2. Core0 wanting to write flash raises the park request and **returns**.
3. On a later main-loop iteration core0 checks whether core1 has parked. If it
   has, core0 disables XIP, writes, re-enables, releases. If not, core0 does
   nothing and tries again next iteration.
4. If core1 has not parked within the deadline, core0 forces it off through
   PSM (`frce_off`), which needs no cooperation from core1 at all, completes in
   a bounded number of cycles, and is then followed by the write. Core1 is
   relaunched afterwards and Lua restarts.

What this buys, stated as the failure case: **a deadlocked core1 delays flight
log flushes and is then reset. It cannot delay the flight state machine, pyro
timing, or sensor sampling, because none of those touch flash.**

That is the whole reason this shape was chosen over a timeout. Flash is needed
for log flushes, config saves, OTA and file uploads — none of which are on the
pyro critical path. Firing is RAM and GPIO only.

### The API names resources, not hardware

Configuration assigns symbolic names. Scripts only ever see those names, so a
script cannot express access it was not granted (L5), and a capability that is
disabled has no table at all (L4).

```ini
[lua]
default = beacon.lua

[lua.output]
beacon = pin18, pwm      ; named output, dimmable
strobe = pin19, digital

[lua.serial]
radio = pins20_21, 9600  ; absent from config -> serial table absent in Lua
```

```lua
-- No pin numbers. No SDK. No way to name the pressure bus or a pyro pin.
output.set("beacon", 40)        -- 40% duty
output.set("strobe", true)

if serial then                  -- nil unless enabled in config
  serial.write("radio", telemetry_line())
end

if flight.state() == flight.DESCENT then
  output.set("strobe", true)
end
```

Config maps a name to a (pin, role) pair; the binding layer resolves the name
against that table and refuses anything not in it. Because Lua never sees a pin
number, the GPIO18/19-as-i2c1 trap is unreachable by construction — no script
can ask for a role, only for a name that config already bound to a vetted role.

---

## Phase 1: Core1 bring-up and the non-blocking park, no Lua

Prove the priority rule before adding a language to the problem. This phase is
the whole safety argument; if it does not hold, nothing later matters.

- `multicore_launch_core1()` running a stub that bumps a heartbeat.
- `multicore_lockout_victim_init()` on core1 for prompt IRQ parking.
- Park routine and its spin are `__not_in_flash_func`; verify via the map file
  that both land in RAM.
- Core0's flash write path becomes **try-write**: request park, poll, and defer
  if core1 is not parked. No blocking call anywhere on that path (L3).
- Deadline exceeded: force core1 off through PSM `frce_off`, complete the
  write, relaunch core1.
- Heartbeat monitor resets core1 independently of any flash activity (L10).

### Success Criteria
- [ ] Host/CI unaffected; MK1B build unchanged
- [ ] Grep gate: no `_blocking` or `_timeout_us` lockout call exists in the tree
- [ ] `nm` gate wired into CI: core1 units reference no `malloc`, `free`,
      `mutex_enter_*`, `spin_lock_*`, `*_claim_*`, `add_alarm_*`, `queue_*`
      or stdio symbol
- [ ] Map file confirms the park routine and its spin are in RAM
- [ ] **A stub that deadlocks with interrupts disabled never prevents a fire.**
      The acceptance test is "the fire always happens", not "the fire is
      prompt": run a simulated flight with core1 deadlocked and confirm every
      deployment occurs. Timing jitter of tens of ms is explicitly acceptable
      and is not a failure.
- [ ] With that stub deadlocked, pyro continuity checks, state transitions and
      a simulated fire all keep correct timing; only log flushes stall
- [ ] After the deadline, core1 is forced off, the write completes, core1 relaunches
- [ ] Flight log writes complete correctly with core1 running normally
- [ ] Measured: park latency, deferral count under load, worst-case core0 delay
      attributable to core1 (target: zero)

---

## Phase 2: Lua VM with hard resource limits

- Vendor Lua 5.4 via FetchContent, built for the ARM target.
- `lua_newstate` with a custom allocator over a fixed arena (target 32 KB,
  tunable). Allocation beyond the arena returns NULL — Lua raises, script dies,
  core0 untouched (L5).
- `lua_sethook` with `LUA_MASKCOUNT` for the instruction budget (L6).
- `lua_atpanic` that longjmps back to the core1 supervisor rather than aborting.
- Strip `io`, `os`, `package`, `debug`; keep `base`, `string`, `math`, `table`.

### Success Criteria
- [ ] `while true do end` is aborted by the budget, core1 survives, core0 never notices
- [ ] A script allocating in a loop hits the arena cap and fails cleanly
- [ ] Arena and VM RAM measured against the 194 KB headroom
- [ ] Lua flash cost measured against the MK1C app slot

---

## Phase 3: Capability model and the output API

The board declares which (pin, role) pairs are *permissible*; configuration
decides which are *enabled* and gives each a name. Both gates must pass.

```c
/* boards/mk1c/board_pins.h — what the HARDWARE permits.
 * Roles, never raw GPIO function numbers. I2C is withheld from 18/19
 * because their I2C instance is i2c1, the pressure bus. */
#define BOARD_HAS_LUA 1
#define BOARD_LUA_PINS                                                        \
    /*  pin  DIGITAL  PWM  SERIAL  BUS */                                     \
    X(18,    1,       1,   0,      0)                                         \
    X(19,    1,       1,   0,      0)                                         \
    X(20,    1,       1,   1,      1)                                         \
    X(21,    1,       1,   1,      1)                                         \
    X(22,    1,       1,   0,      0)
```

Two compile-time checks, in the binding layer so no board can opt out:

1. No pin in `BOARD_LUA_PINS` appears in the board's pyro, sensor or buzzer set.
2. No pin whose bus instance is `BOARD_I2C_INST` may carry the BUS role.

Then at startup core0 walks the config, resolves each name against the board
table, and builds the Lua environment. Names that fail either gate are dropped
with a logged reason; capabilities with no enabled resources get no table (L4).

Lua API — no pin numbers, no SDK types (L1, L5):

```lua
output.set(name, value)    -- true/false, or 0-100 for a pwm-role output
output.get(name)
output.list()              -- what this script is actually allowed to touch
```

`gpio_put` is a single SIO register write taking no lock, so core1 performs it
directly. The name-to-pin resolution happened once, on core0, at config load.

### Success Criteria
- [ ] Host tests: a name bound to a forbidden pin is rejected at config load
- [ ] A board header exposing a pyro pin fails the build
- [ ] A board header granting BUS to GPIO18 or 19 fails the build
- [ ] With no outputs configured, `output` is nil in the Lua environment
- [ ] `output.list()` returns exactly the configured names, nothing more
- [ ] No pin number or SDK symbol appears anywhere in the Lua-visible surface —
      grep gate over the binding table
- [ ] Scope FIRE_A/FIRE_B/ARM_TOGGLE and SDA1/SCL1 while a script calls
      `output.set` with every name and with fuzzed junk names — no activity on
      any of them, MS5607 still reading correctly

---

## Phase 4: Read-only state APIs

Core0 publishes a snapshot; core1 reads it without ever blocking core0. A
seqlock (write counter either side of the payload) is enough and is lock-free
for the writer.

- `sensor.pressure_pa()`, `sensor.altitude_cm()`, `sensor.temperature_c()` —
  last values the primary state machine read
- `flight.state()` → enum matching `flight_state_t`
- `flight.time_ms()`, `flight.max_altitude_cm()`
- `pyro.status()` → armed / fired / continuity / fault enums (L11: read-only)

### Success Criteria
- [ ] Core0 publish path measured; must not be a blocking operation
- [ ] Core1 reading during a publish gets a consistent snapshot, never a torn one
- [ ] No pyro-mutating API exists anywhere in the binding table — grep gate
- [ ] Snapshot is published by core0 and read by core1 with no peripheral
      access on core1

---

## Phase 5: Serial and bus, driven by core1 directly

Core1 drives `uart1` and `i2c0` itself — no round trip through core0. What
keeps that safe is L2, not avoidance:

- Core0 calls `uart_init`/`i2c_init` and claims any DMA channel **before**
  launching core1. Core1 never claims anything.
- Core1 uses timeout variants only: `uart_write_blocking` is permitted because
  it polls a FIFO and takes no lock, but `i2c_write_timeout_us` is required
  over `i2c_write_blocking` so a stuck slave cannot hold the VM forever.
- No transfer allocates. Buffers come from the Lua arena or a preallocated
  static.

```lua
serial.write(name, str)
serial.read(name, max)
bus.write(name, addr, str)
bus.read(name, addr, n)          -- returns nil, "timeout" rather than hanging
```

Both tables are absent unless configuration enables them (L4). `uart0` and
`i2c1` are not addressable by any name, because no board table grants those
roles on their pins.

A stuck peripheral is now a core1 problem only: the VM stalls, the watchdog
notices, core1 is reset. Core0 never waited on it.

### Success Criteria
- [ ] `nm` gate: no forbidden symbol reachable from the core1 units (L2)
- [ ] Every peripheral resource is claimed by core0 before `multicore_launch_core1`
- [ ] A deliberately stuck I2C slave stalls only core1; pyro timing matches the
      core1-idle baseline, and core1 is reset by the heartbeat
- [ ] A script issuing back-to-back large writes does not measurably delay the
      flight loop
- [ ] With the capability disabled, the table is nil rather than erroring

---

## Phase 6: Script storage, events, and configuration

- Scripts live at `/lua/<name>.lua`, uploaded through the existing file route.
- Config gains: per-event script assignment, a default script, the pin roles,
  bus enables, and the FS-I/O enable. Extend `config_fields.h`'s X-macro.
- Core0 reads a script into RAM and hands the buffer to core1 — core1 never
  calls littlefs (L4).
- Event dispatch: core0 already emits `EVT_LAUNCH`, `EVT_ARMED`, `EVT_APOGEE`,
  `EVT_PYRO1_FIRE`, `EVT_LANDING` and friends. These go into a queue core1
  drains, invoking the assigned script.
- The default script gets `init()` once and `tick()` repeatedly, so it does not
  depend on any event.

### Success Criteria
- [ ] Several scripts stored, listed and individually assigned
- [ ] Each event invokes exactly its assigned script, once per occurrence
- [ ] The default script runs with no events at all
- [ ] A missing or syntactically invalid script logs an error and does not stop the others

---

## Phase 7: WebSocket console

- `GET /lua/console` with `Upgrade: websocket` in `http_server.c`
- RFC6455 handshake: SHA1 of key + GUID, base64, `101 Switching Protocols`
- Text frames only; no fragmentation, no binary, no compression
- Lines queue to core1, evaluated in the live VM; output queues back
- Console-originated errors never kill the default script

### Success Criteria
- [ ] Handshake against a real browser
- [ ] `print()` from a script reaches the console
- [ ] An error at the console leaves the default script running
- [ ] Console disconnect mid-evaluation does not wedge core1

---

## Phase 8: Filesystem I/O for Lua

Disabled by default. All writes are requests to core0 (L6), and the table is absent unless enabled (L4).

- `fs.read(path)`, `fs.append(path, s)`, `fs.list()`
- Writes are queued, rate-limited, and **refused outside PAD_IDLE and LANDED** —
  a script appending at 100 Hz during ascent would otherwise trigger constant
  sector erases and stall itself and the log
- Quota so a script cannot fill the 8 MB filesystem and starve flight logging

### Success Criteria
- [ ] A script writing in a tight loop cannot delay flight logging
- [ ] Writes refused during ASCENT/DESCENT with a clear error
- [ ] Quota enforced; flight log always has room

---

## Phase 9: Configuration tab and documentation

- New web UI tab: script upload/list/delete, event assignment, pin roles, bus
  enables, FS enable, console
- The tab shows the forbidden set greyed out with the reason, so the constraint
  is visible rather than a mystery rejection
- Document the API, the stall behaviour, and the invariants

### Success Criteria
- [ ] Playwright coverage for the new tab in all three mock modes
- [ ] Forbidden pins not selectable in the UI *and* rejected by firmware
- [ ] `node --check`, clang-format, pmccabe all pass

---

## Phase 10: Adversarial soak

The phase that decides whether this ships.

- [ ] Infinite loop in the default script — full simulated flight still correct
- [ ] Script OOMs repeatedly — core0 unaffected, log intact
- [ ] Script writes every pin number 0–29 continuously — scope confirms no pyro activity
- [ ] Core1 deadlocked with interrupts off for a whole simulated flight — pyro
      timing identical to the core1-idle baseline, log flushes resume after the
      forced reset
- [ ] Flash write attempted while core1 is mid-script, repeatedly, for a whole
      flight — no missed log data once core1 parks normally
- [ ] A script tries every capability name while every capability is disabled —
      all tables nil, no crash, default script unaffected
- [ ] Core1 spins in a peripheral delay loop for a whole flight — pyro timing
      matches the core1-idle baseline
- [ ] Core1 killed at a random point 100 times; core0 never observed blocked,
      and no run leaves a lock held
- [ ] Console spammed during ascent — pyro timing unaffected
- [ ] Measured: worst-case core1 stall, worst-case core0 delay attributable to Lua (target: zero)

---

## Open Questions

1. **Instruction budget per invocation** — an event handler and a `tick()` want
   different budgets. Start generous and tune once the soak test runs?
2. **Arena size.** 32 KB is a guess against 194 KB headroom. Worth deciding
   whether Lua may grow into the flight-log buffer's reserve, or must not.
3. **Park deadline.** How long core0 tolerates deferred flash writes before
   forcing core1 off. Too short and a busy script loses its VM needlessly; too
   long and the flight log falls behind. Measure the normal park latency in
   Phase 1 and set the deadline well above it.
4. **Should the default script keep running through ASCENT/DESCENT?** Night-launch
   LEDs say yes, and with core1 unable to touch hardware the risk is mostly
   core0's I/O service budget. A config option to suspend Lua in flight remains
   the escape hatch.
5. **How far to take the `nm` gate.** Checking direct references is easy;
   proving nothing in the transitive call graph allocates is harder, since a
   Lua binding could reach a library that mallocs. A link-time call-graph dump
   would be thorough but fragile. Start with direct references plus a reviewed
   binding list?
6. **Script provenance.** Nothing authenticates an uploaded script. Same trust
   level as the OTA path, which is also unauthenticated — worth deciding once,
   for both.

## References

- `src/board_if.h`, `boards/README.md` — board capability pattern this extends
- `DESIGN.md` §9 invariants, §7.1 authority — why L8 exists
- `DECISIONS.md` #2 — no flash I/O in the USB callback path; same hazard class
- RP2040 datasheet §2.8.3 (XIP), SDK `pico_multicore` lockout API
