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

- **MK1C as built has one reachable spare pin.** GPIO2,3,4,5,9,10,13,14,15 are
  unconnected on the PCB — no pad. GPIO18–21 land on J3, which is not fitted.
  GPIO22 goes to J1.6. GPIO0/1 are the telemetry UART on J1.4/J1.5.
  **Decision: J3 gets populated**, giving Lua GPIO18–22, where GPIO20/21 can be
  `uart1` or `i2c0`. Telemetry keeps `uart0`.

- **Core1 cannot execute while core0 writes flash.** XIP must be disabled for
  `flash_range_erase`/`flash_range_program`, and Lua's interpreter is far too
  large to run from RAM. **Decision: multicore lockout**, accepting that Lua
  stalls up to ~45 ms on a sector erase. This is the single largest behavioural
  caveat and every timing-sensitive Lua API must tolerate it.

- **`multicore_lockout_start_blocking()` can hang core0 forever** if core1 is
  wedged with interrupts disabled. That directly violates the priority rule, so
  the timeout variant is mandatory — see invariant L1.

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

## Safety Invariants

These are to Lua what `DESIGN.md` §9 is to the pyro circuit. Numbered for
reference in code comments.

**L1. Core0 never blocks on core1.** Every handshake uses a timeout. On expiry
core0 resets core1 and continues. `multicore_lockout_start_blocking()` is
banned; only the `_timeout_us` variant is used.

**L2. Lua can never name a forbidden pin.** The allowlist is declared by the
board and statically asserted to exclude every pyro, sensor and buzzer pin.

**L3. Peripheral instances are partitioned, not just pins.** Lua may touch
`uart1` and `i2c0` only. `uart0` (telemetry) and `i2c1` (pressure) are never
reachable, even if a pin number were to slip through L2.

**L4. Lua never writes flash.** All filesystem writes are requests to core0.

**L5. Lua's heap is a fixed arena.** Exhaustion fails the script, never core0.

**L6. Every Lua invocation has an instruction budget.** Overrun aborts the
script and logs it.

**L7. Core1 failure never resets core0.** A hang, panic or OOM on core1 is
contained: core0 detects it by heartbeat and restarts core1.

**L8. Lua state is read-only with respect to flight.** No Lua API can arm,
fire, disarm, change a pyro mode, or alter the flight state machine.

## What We're NOT Doing

- Not giving Lua any write path to pyro, and not exposing a "fire" API behind a
  flag. `DESIGN.md` invariant 11 says every fire command names an authority and
  there is no third source; a user script would be exactly that third source.
- Not running Lua on MK1B — it does not fit.
- Not making Lua real-time. Flash activity on core0 stalls it by tens of ms.
- Not sharing `lfs_t` across cores.
- Not implementing a Lua debugger, `require` of arbitrary modules, `os.execute`,
  or the `debug` library.
- Not letting Lua survive a reset with state. Each boot starts clean.

## Architecture

```
  CORE 0  (pyro application, priority)      CORE 1  (Lua only)
  ------------------------------------      ---------------------------
  flight state machine                      Lua 5.4 VM
  HAL, pyro, pressure, buzzer               fixed-arena allocator
  USB, lwIP, HTTP, WebSocket                instruction-budget hook
  littlefs  (sole owner)                    sandboxed C bindings
                                            uart1 / i2c0 / GPIO18-22
        |                                           |
        |  state snapshot (seqlock, lock-free) ---> |
        |  <--- request queue (FS, console out)     |
        |  lockout + timeout during flash writes -> |
```

Core1 drives its own allowlisted peripherals directly — no round trip through
core0 — because those peripherals belong to it exclusively. Only the filesystem
and console output need core0.

---

## Phase 1: Core1 bring-up and lockout safety, no Lua

Prove the priority rule before adding a language to the problem.

- `multicore_launch_core1()` running a stub that busy-loops and bumps a
  heartbeat counter.
- `multicore_lockout_victim_init()` on core1.
- Wrap the littlefs write path (`littlefs_driver.c` prog/erase) and the OTA path
  in lockout start/end, using `multicore_lockout_start_timeout_us()`.
- On timeout: reset core1, log it, continue. Never retry forever.
- Core0 heartbeat monitor: if core1's counter stops advancing, reset core1.

### Success Criteria
- [ ] Host/CI unaffected; MK1B build unchanged
- [ ] A stub that deliberately disables interrupts and spins does **not** hang
      core0 — core0 times out, resets core1, and the flight loop keeps running
- [ ] Flight log writes complete correctly with core1 running
- [ ] Measured worst-case core1 stall during a sector erase, recorded

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

## Phase 3: Sandbox and the GPIO API

The board declares what Lua may touch, and the compiler proves it is safe:

```c
/* boards/mk1c/board_pins.h */
#define BOARD_HAS_LUA 1
#define BOARD_LUA_GPIO_ALLOW {18, 19, 20, 21, 22}
#define BOARD_LUA_UART      uart1   /* never uart0: telemetry */
#define BOARD_LUA_I2C       i2c0    /* never i2c1: pressure   */
```

A static assertion in the binding layer checks the allowlist against the board's
forbidden set — pyro, sensor and buzzer pins — so a board that tries to expose
GPIO17 fails to compile (L2). Every API entry point re-validates at runtime,
because a compile-time check does not cover a pin number computed in Lua.

API: `gpio.setup(pin, mode)`, `gpio.write(pin, v)`, `gpio.read(pin)`.

### Success Criteria
- [ ] Host tests for the allowlist: every forbidden pin rejected, every allowed pin accepted
- [ ] A board header exposing a pyro pin fails the build
- [ ] `gpio.write(17, 1)` from Lua returns an error and changes no pin state
- [ ] Scope FIRE_A/FIRE_B/ARM_TOGGLE while a script hammers `gpio.write` across all 30 pin numbers — no activity

---

## Phase 4: Read-only state APIs

Core0 publishes a snapshot; core1 reads it without ever blocking core0. A
seqlock (write counter either side of the payload) is enough and is lock-free
for the writer.

- `sensor.pressure_pa()`, `sensor.altitude_cm()`, `sensor.temperature_c()` —
  last values the primary state machine read
- `flight.state()` → enum matching `flight_state_t`
- `flight.time_ms()`, `flight.max_altitude_cm()`
- `pyro.status()` → armed / fired / continuity / fault enums (L8: read-only)

### Success Criteria
- [ ] Core0 publish path measured; must not be a blocking operation
- [ ] Core1 reading during a publish gets a consistent snapshot, never a torn one
- [ ] No pyro-mutating API exists anywhere in the binding table — grep gate

---

## Phase 5: UART and I2C

Only `uart1` and only `i2c0`, only on the configured pins, only when enabled
(L3). Both are disabled by default.

- `uart.open(baud)`, `uart.write(s)`, `uart.read(n, timeout_ms)`
- `i2c.open(hz)`, `i2c.write(addr, s)`, `i2c.read(addr, n)`

Every call returns `nil, err` rather than blocking indefinitely, because a
lockout freeze can land mid-transfer and the caller must be able to see that.

### Success Criteria
- [ ] Bus instance is compile-time fixed; `uart0`/`i2c1` unreachable from any path
- [ ] A transfer interrupted by a lockout freeze reports an error, does not hang
- [ ] With the bus disabled in config, every call returns an error

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

Disabled by default. All writes are requests to core0 (L4).

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
- [ ] Core1 wedged with interrupts off — core0 times out, resets it, flight continues
- [ ] Console spammed during ascent — pyro timing unaffected
- [ ] Measured: worst-case core1 stall, worst-case core0 delay attributable to Lua (target: zero)

---

## Open Questions

1. **Does J3 get populated on serial #1, or only on later boards?** Phases 5
   onward need those pins on real hardware; earlier phases do not.
2. **Instruction budget per invocation** — an event handler and a `tick()` want
   different budgets. Start generous and tune once the soak test runs?
3. **Arena size.** 32 KB is a guess against 194 KB headroom. Worth deciding
   whether Lua may grow into the flight-log buffer's reserve, or must not.
4. **Should the default script keep running through ASCENT/DESCENT?** Night-launch
   LEDs say yes. If any script ever proves capable of disturbing timing, a
   config option to suspend Lua in flight is the escape hatch.
5. **Script provenance.** Nothing authenticates an uploaded script. Same trust
   level as the OTA path, which is also unauthenticated — worth deciding once,
   for both.

## References

- `src/board_if.h`, `boards/README.md` — board capability pattern this extends
- `DESIGN.md` §9 invariants, §7.1 authority — why L8 exists
- `DECISIONS.md` #2 — no flash I/O in the USB callback path; same hazard class
- RP2040 datasheet §2.8.3 (XIP), SDK `pico_multicore` lockout API
