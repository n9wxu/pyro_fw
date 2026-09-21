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
core0 resets core1 and continues.

**L2. Core1 touches no peripheral, ever.** No GPIO, no UART, no I2C, no DMA
channel, no peripheral register of any kind. Core1 runs Lua and nothing else.
All I/O is performed by core0, which already owns the hardware.

**L3. Core1 runs with no interrupt sources.** No IRQ handlers are installed on
core1 and its NVIC stays empty. This removes interrupt latency, shared-IRQ
hazards and re-entrancy from the Lua core entirely.

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
- Not letting core1 touch a peripheral, a DMA channel, or an interrupt.
- Not exposing the Pico SDK — no pin numbers, no instances, no SDK constants
  or types anywhere in the Lua-visible surface.
- Not sharing `lfs_t` across cores.
- Not implementing a Lua debugger, `require` of arbitrary modules, `os.execute`,
  or the `debug` library.
- Not letting Lua survive a reset with state. Each boot starts clean.

## Architecture

Core1 is a pure computation engine. It has no interrupts, no peripherals, and
no way to name hardware. Everything it wants done, core0 does.

```
  CORE 0  (pyro application, priority)   CORE 1  (Lua, compute only)
  -----------------------------------    ----------------------------
  flight state machine                   Lua 5.4 VM
  ALL peripheral I/O                     fixed-arena allocator
  littlefs (sole owner)                  instruction-budget hook
  DMA for bulk transfers                 NO interrupts
  services the Lua I/O ring,             NO peripheral access
    bounded work per iteration           NO SDK symbols linked

        |  state snapshot (seqlock, lock-free)  -->  |
        |  <--  I/O request ring                     |
        |  I/O completion ring                  -->  |
        |  park request flag                    -->  | checked in the Lua hook
```

### Parking without interrupts

The SDK's `multicore_lockout_victim_init()` cannot be used: it installs a SIO
FIFO IRQ handler on core1, and L3 forbids interrupts there. The replacement is
cooperative and is a deliberate deviation from the SDK mechanism:

1. Core0 needs to write flash. It sets `park_request` and waits.
2. Core1's instruction-count hook — already present for the budget in L8 —
   sees the flag, calls a `__not_in_flash_func` park routine, sets
   `park_acked`, and spins in RAM until released.
3. Core0 sees the ack, disables XIP, writes flash, re-enables, clears the flag.
4. If the ack does not arrive within the timeout, core0 resets core1 and
   proceeds (L1). Core0 is never stuck.

Response latency is bounded by the hook interval rather than by interrupt
latency, which is acceptable because real-time behaviour is not a requirement.
Bindings must not loop, so the hook is always reached — which holds naturally
here, since no binding does I/O; they only post to a ring.

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

## Phase 1: Core1 bring-up, cooperative parking, no Lua

Prove the priority rule before adding a language to the problem.

- `multicore_launch_core1()` running a stub loop that bumps a heartbeat and
  polls `park_request` at a fixed interval, standing in for the Lua hook.
- Park routine and the spin it parks in are both `__not_in_flash_func`.
- Core1 starts with its NVIC empty; assert no IRQ is enabled on core1 (L3).
- Core0 wraps the littlefs write path (`littlefs_driver.c` prog/erase) and the
  OTA path in park-request / release, with a timeout.
- On timeout: reset core1, log it, continue. Never retry forever (L1).
- Heartbeat monitor: if core1's counter stops advancing, reset core1 (L10).

### Success Criteria
- [ ] Host/CI unaffected; MK1B build unchanged
- [ ] A stub that stops polling does **not** hang core0 — core0 times out,
      resets core1, and the flight loop keeps running
- [ ] Core1's NVIC verified empty at runtime
- [ ] Flight log writes complete correctly with core1 running
- [ ] Measured: worst-case park latency, and worst-case core0 delay
      attributable to core1 (target: the timeout, and only on a fault)

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

Every call is a post to the I/O ring; core0 performs the pin write (L2).

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
      access and no interrupt on core1

---

## Phase 5: Serial and bus, mediated by core0 and DMA

Still no peripheral access from core1 (L2). Lua posts a request; core0 runs the
transfer, using DMA for anything bulky so wire time is not core0's problem (L9).

```lua
serial.write(name, str)          -- queued; returns bytes accepted
serial.read(name, max)           -- from a core0-side receive buffer
bus.write(name, addr, str)
bus.read(name, addr, n)
```

Both tables are absent unless configuration enables them (L4). `uart0` and
`i2c1` are not addressable by any name, because no board table grants those
roles on their pins.

Core0 services the ring with a bounded per-iteration budget. A script asking
for more than the budget simply waits — the flight loop does not.

### Success Criteria
- [ ] Transfers run on core0/DMA; core1 never touches a peripheral register
- [ ] Per-iteration service budget enforced and measured
- [ ] A script issuing back-to-back large writes does not measurably delay the
      flight loop (compare pyro timing against a Lua-idle baseline)
- [ ] With the capability disabled, the table is nil rather than erroring
- [ ] A transfer spanning a flash-write park completes or reports an error,
      never hangs

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
- [ ] Core1 stops polling the park flag — core0 times out, resets it, flight continues
- [ ] Core1 verified to have raised zero interrupts for the whole soak
- [ ] A script tries every capability name while every capability is disabled —
      all tables nil, no crash, default script unaffected
- [ ] Console spammed during ascent — pyro timing unaffected
- [ ] Measured: worst-case core1 stall, worst-case core0 delay attributable to Lua (target: zero)

---

## Open Questions

1. **Instruction budget per invocation** — an event handler and a `tick()` want
   different budgets. Start generous and tune once the soak test runs?
2. **Arena size.** 32 KB is a guess against 194 KB headroom. Worth deciding
   whether Lua may grow into the flight-log buffer's reserve, or must not.
3. **Hook interval.** It sets both the instruction budget granularity and the
   park latency. Too coarse and core0 waits longer for a park ack; too fine and
   Lua throughput suffers. Measure in Phase 1 before fixing it.
4. **Should the default script keep running through ASCENT/DESCENT?** Night-launch
   LEDs say yes, and with core1 unable to touch hardware the risk is mostly
   core0's I/O service budget. A config option to suspend Lua in flight remains
   the escape hatch.
5. **Script provenance.** Nothing authenticates an uploaded script. Same trust
   level as the OTA path, which is also unauthenticated — worth deciding once,
   for both.

## References

- `src/board_if.h`, `boards/README.md` — board capability pattern this extends
- `DESIGN.md` §9 invariants, §7.1 authority — why L8 exists
- `DECISIONS.md` #2 — no flash I/O in the USB callback path; same hazard class
- RP2040 datasheet §2.8.3 (XIP), SDK `pico_multicore` lockout API
