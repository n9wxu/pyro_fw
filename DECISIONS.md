# Design Decisions

Decisions made during v2.0 architecture planning. Each decision records the
rationale and the alternatives considered.

## Sensor & Sampling

### DD-001: 50Hz Uniform Pressure Sampling
- **Decision:** Sample pressure at 50Hz (20ms) for all flight states except LANDED (1Hz).
- **Settings:** MS5607 OSR=2048 (±0.30m noise), BMP280 P×8/T×1 (±0.22m noise).
- **Rationale:** Both sensors can sustain 50Hz at this accuracy. A uniform rate
  simplifies the IIR filter and makes the system more predictable.
- **Alternatives:** Per-state rates (100Hz pad, 10Hz ascent, 20Hz descent) —
  rejected because BMP280 maxes out at 26Hz with current oversampling, and
  varying rates complicate filter behavior.

### DD-005: Non-Blocking Sensor State Machine
- **Decision:** Remove all `sleep_ms()` from sensor drivers. Use a state machine
  that sends a conversion command, returns, and checks back next tick.
- **Rationale:** `sleep_ms(10)` × 2 = 20ms blocking per read. During that time:
  no USB polling, no buzzer updates, no pyro timing. Violates non-blocking design.
- **Alternatives:** DMA alone — rejected because DMA handles bus transfers (~0.1ms)
  but not the 8ms conversion wait in the sensor.

## Pressure FIFO Architecture

### DD-002: Autonomous Sensor Pipeline
- **Decision:** Core1 (RP2040) or ISR+DMA (ESP32-C3, STM32C011) runs the sensor
  pipeline. The flight software on Core0/main loop only consumes data.
- **Rationale:** Decouples sensor timing from CPU availability. XIP stalls on Core0
  during flash writes do not affect sensor sampling.

### DD-003: Lock-Free SPSC Pressure FIFO (16 entries)
- **Decision:** Single-producer single-consumer ring buffer, 16 entries, 320ms buffer at 50Hz.
- **Rationale:** 320ms covers worst-case flash sector erase (45-100ms) with 3× margin.
  Lock-free because only one writer (ISR/Core1) and one reader (main loop).

### DD-004: Sensor Code in RAM (RP2040)
- **Decision:** Use `__not_in_flash_func()` for sensor ISR code on RP2040.
- **Rationale:** When Core0 does flash writes, XIP is disabled. Core1 must run from
  RAM to continue sampling. I2C registers are memory-mapped (not flash), so
  I2C transactions work during XIP stalls.

### DD-006: FIFO Is the Portability Boundary
- **Decision:** The flight software only sees the FIFO consumer API. The producer
  mechanism is entirely platform-specific, hidden inside the HAL.
- **Rationale:** RP2040 uses Core1, ESP32-C3 uses timer ISR, STM32C011 uses TIM+DMA.
  Same consumer code on all platforms.

### DD-007: HAL Pressure API Is FIFO-Based
- **Decision:** Replace `hal_pressure_read()` with `hal_pressure_get_buffer()` /
  `hal_pressure_release_buffer()` delivering 5-sample batches every 100ms.
- **Rationale:** Batch delivery enables CPU sleep between processing windows.
  Flight software processes all samples in a burst, then sleeps.

## Flash & Data Logging

### DD-008: Allow Flash Writes During Ascent
- **Decision:** The incremental CSV logger may write to flash during all flight states.
- **Rationale:** With the pressure FIFO (DD-003), XIP stalls from flash writes are
  absorbed. Worst case: sector erase (100ms) causes 5 samples to queue in the
  FIFO. Core0 wakes and processes the burst. No samples lost. Apogee detection
  delayed by at most one 20ms sample — negligible.
- **Alternatives:** Buffer in RAM during ascent, flush after pyro fire — rejected
  because it requires large buffers for high-altitude flights (21KB for 10,000ft)
  and limits portability to low-RAM MCUs.

### DD-014: Force Flush at Apogee
- **Decision:** Force a flash write of buffered flight data when apogee is detected.
- **Rationale:** If the rocket lawn-darts after drogue failure, power may be lost
  on impact. Flushing at apogee ensures altitude and apogee event are on flash.

## Platform Targets

### DD-009: Three Platform Targets
- **Decision:** Support RP2040 (reference), ESP32-C3 (full/wireless), STM32C011 (lite).
  Drop ATtiny402.
- **Rationale:** ATtiny402 has 256B RAM — insufficient for flight software + buffers
  without extreme compromises. The STM32C011 (6KB RAM, $0.50) is the minimum
  viable platform. All three targets have I2C, UART, ADC, timer, and DMA.

### DD-012: Pin Assignments Per-Platform
- **Decision:** Remove hardware pin table from SPECIFICATION.md. Pin assignments
  are defined in each platform's HAL header.
- **Rationale:** Pin mapping is platform-specific. The specification describes the
  logical interface (I2C sensor, 2 pyro channels, buzzer, UART telemetry).

## Safety

### DD-013: Backup Apogee Timer
- **Decision:** Force apogee detection if no apogee detected within a configurable
  time (default 30s, range 10-120s) after pyros are armed.
- **Rationale:** If the pressure sensor fails or produces garbage during coast,
  the primary speed-based apogee detection may never trigger. The backup timer
  ensures chutes deploy even with sensor failure.
- **Prerequisite:** Timer starts ONLY after confirmed launch (DD-016) AND
  pyro arming (DD-017). Never on the ground.

### DD-016: Strengthened Launch Confirmation
- **Decision:** Launch requires ALL of:
  1. Filtered altitude exceeds 10 meters
  2. Altitude gained 10m within 2 seconds
  3. Vertical speed > 5 m/s at detection time
- **Rationale:** Prevents false launch from barometric drift, weather fronts, or
  thermal expansion. A real rocket at 10m altitude is traveling at 15+ m/s.
- **Alternatives:** Altitude-only (current) — vulnerable to false launch from
  slow pressure changes while sitting on the pad for hours.

### DD-017: Arming Requires Confirmed Motor Burn
- **Decision:** Pyro arming requires max vertical speed during ASCENT exceeded 20 m/s.
- **Rationale:** After a false launch (from drift), speed is ~0 m/s. Without this
  gate, arming happens immediately (speed < 10 m/s), then apogee fires (speed ≈ 0).
  This gate blocks the entire false-launch → ground-fire chain.

### DD-015: Landing Timeout for Elevation Mismatch
- **Decision:** Detect landing if descent has lasted 60 seconds and speed is below
  5 m/s, regardless of AGL altitude.
- **Rationale:** If the rocket lands at an elevation significantly above the launch
  pad (mesa, hillside), the 30m AGL check may never pass. The timeout ensures
  the system transitions to LANDED and begins post-flight operations.

## Configuration & Features

### DD-010: beep_mode Deferred to V2 X-Macro Config
- **Decision:** The `beep_mode` config field (digits vs hundreds) is not implemented
  in v1.5. It will be added via the X-macro config table in V2 Task 1.
- **Rationale:** The X-macro system generates parser, serializer, and test
  automatically. Adding it in v1.5 would require manual parser code that
  is immediately replaced.

### DD-011: Serial Ground Test Replaces GPIO 8 Jumper
- **Decision:** Remove GPIO 8 test mode from specification. Ground testing is
  performed via serial commands through the 3.5mm TRRS jack.
- **Rationale:** Works on all 3 MCU platforms. Supports sequenced multi-device
  testing via bus master. More features than the jumper approach. Frees GPIO 8
  from dual-use complexity.

### DD-018: Lua Resources Are One Interface Table, Not Four APIs
- **Decision:** `lua_platform.h` no longer declares output, input, serial and
  pixel families. Whoever configures the hardware publishes
  `{name, kind, vtable, ctx}` entries into `lua_iface.h`'s table, and Lua
  resolves by name **and kind**. Boards add their own features through the
  weak `board_lua_publish()` hook rather than by widening the header.
- **Rationale:** The kind is part of the lookup, so a wrong combination is
  unreachable rather than refused — `output.set()` on an input-published pad
  finds nothing, because no output vtable was ever installed. Dimmability
  lives in the vtable for the same reason. This is the argument already made
  by the half-bridge PIO program, where shoot-through is unencodable rather
  than avoided: a check can be forgotten, a missing pointer cannot be called.
- **Cost:** Indirect calls are invisible to `prove_core0.py`, which builds its
  graph from `bl` instructions. Taken alone the refactor would have severed
  the graph and left `core1_main acquires nothing` passing while proving
  strictly less. The checker now folds `*_vt` entries in as reachable from
  core1, and fails an image that links `lua_iface_publish` with no `*_vt`
  symbol.

### DD-019: A Released Pyro Channel Is Mocked, Not Guarded
- **Decision:** The flight software's per-channel pyro operations (`fire`,
  `get`, `fault`) are a table installed once at boot from the pin assignment.
  A released channel gets a mocked table that touches no hardware; the real
  operations are not reachable for it. The two shared entry points
  (`hal_pyro_sample`, `hal_pyro_update`) return early once BOTH channels are
  released, because they drive the common.
- **Rationale:** A released pad belongs to Lua on core1, and core0 writing it
  fights for the same SIO register — MK1B was stamping a released output low
  on every sample. Making that a check at each call site means every future
  call site has to remember it; making it a pointer that does not go there
  means none of them can get it wrong. Same argument as DD-018.
- **Nothing is silent:** every mocked operation is written to the flight log
  as a `MOCK` row beside the event that commanded it, sent on the telemetry
  downlink, and counted on `/api/status`. A log showing `PYRO1` with no note
  beside it would record an ignition that did not occur.
- **A released channel reports open, not good.** There is no igniter circuit
  the flight software controls there; reporting continuity would let it arm
  and then "fire" a channel that cannot.

