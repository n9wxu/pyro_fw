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

### DD-020: A Pad Has One Owner, and the Claim Is What Yields the vtable
- **Decision:** `src/pad_claim.c` holds one owner per pad. `pin_store_claim_pads()`
  walks the assignment once and writes each pad's owner — one array, one write
  per pad. Afterwards, `pyro_release_claim()` installs a channel's real methods
  only if it can claim that channel's pads, and `lua_iface_publish()` refuses a
  resource unless Lua can claim every pad it drives. Claims are all or nothing.
- **Rationale:** The pyro table and the Lua interface table were independent, and
  only `pin_assign_validate()` plus the order of two boot calls kept them
  disjoint — a check and a convention. A pad in both means two cores driving one
  FET gate. Making the claim the currency means the second table's entry cannot
  be created at all: nothing compares the tables, there is simply one pad and one
  owner, and the claim was already spent. Same argument as DD-018 and DD-019,
  applied to the thing those two tables share.
- **All or nothing** is what stops a half-claimed bridge: a resource driving a
  free pad and a held one gets neither, rather than one gate it owns and one it
  does not.
- **Reported:** `/api/status` carries what the claim decided (`pyro1_real`,
  `pyro2_real`) beside what the assignment asked for, so a disagreement is
  visible rather than inferred.

### DD-021: PYR-SAFE-02 Dropped; Both Channels May Deploy on One Event
- **Decision:** Remove PYR-SAFE-02 ("shall not fire two channels
  simultaneously"). A low flight may need drogue and main out together, and a
  requirement forbidding it made the one configuration that handles that case
  non-compliant.
- **Replaced by PYR-DEPLOY-01:** both channels may deploy on a single flight
  event.
- **And by PYR-DEPLOY-02, which is not the same rule wearing a new name.** The
  two channels must still not be *energised at the same instant*, because they
  share one common element: two igniters in parallel draw through one FET and
  one fuse. On MK1B that path is a 1.5 A self-resetting PTC, and the combined
  draw can trip it and fire **neither**. On MK1A it is an 8 A one-shot fuse
  with more headroom but no recovery.
- **So `hal_pyro_is_firing()` stays, and its reason has changed.** It was
  incidentally enforcing a safety requirement; it is now deliberately enforcing
  a current limit. Anyone tempted to simplify it away should read this first.
- **The old test was proving almost nothing.**
  `test_PYR_SAFE_02_no_simultaneous_fire` asserted the two fire times differ,
  but `run_sim()` clears `mock_pyro.firing` before every step, so the 500 ms
  hardware separation it measured was one simulation step. It is now
  `test_PYR_DEPLOY_01_low_flight_fires_both` and asserts what the requirement
  actually says: both deploy, close enough together to be one event.

### DD-022: The Backup Apogee Timer Is Removed
- **Decision:** Delete `backup_apogee_expired()`, the `backup_timer` config
  field, and requirements FLT-APO-05/06. DD-013 is superseded.
- **Why:** It only helps when the timer value is right, and a wrong value fires
  during ascent. A backup that can destroy the rocket it is backing up is worse
  than no backup.
- **It could not help in the case it was written for anyway.** It was keyed on
  `armed_time` and evaluated only inside `detect_ascent`, so a board whose
  sensor failed before arming never ran it -- and a failed sensor is precisely
  why a board would not arm.
- **Nothing replaces it, deliberately.** There is now no timeout on ASCENT: if
  the sensor dies mid-flight, the machine stays there. That costs the log, not
  the rocket. Any replacement would need the sensor to notice that the sensor
  has failed.
- **`armed_time` stays.** It is the only record that arming preceded apogee,
  which `test_integration.c` asserts as an ordering guarantee.
- **`max_coast_s` remains dead.** It is parsed, stored and read by nothing. It
  was not pressed into service as a substitute timeout here, because that would
  reintroduce exactly what this decision removes.

### DD-023: Descent Phase Comes From The Rocket, Not The Firing Log
- **Decision:** `FALLING` / `DROGUE_DESCENT` / `CHUTE_DESCENT` advance on the
  measured descent rate holding steady, never on which channel was commanded.
  Landing is detected in all three. The state numbers are unchanged, so old
  flight logs still read correctly.
- **The bug this kills:** `detect_falling` exited only on `pyro1_fired` and
  `detect_drogue_descent` only on `pyro2_fired`. A channel with no continuity,
  a channel set to `NONE`, or the two firing out of order each parked the
  machine for the rest of the flight, and since `LANDED` was reachable only
  from `CHUTE_DESCENT`, `hal_log_stop()` was never called. **Observed live on
  MK1C**, where it also blocked its own OTA.
- **Why a rate and a dwell:** a working canopy is a descent rate that has
  stopped changing; a failed one is a rate that has not. Free fall gains about
  11.8 m/s over the 1.2 s dwell, which breaks the stability tolerance at every
  rate a canopy could explain -- that is what stops the descent through the
  main band just after apogee from reading as a deployed main.
- **Settling in the fast band is not success.** A rocket at terminal velocity
  has a perfectly steady rate. An earlier draft treated "settled" as "a canopy
  is working" and so disabled the emergency ladder in exactly the shredded-
  drogue case it exists for. The ladder is keyed on the band, not on steadiness.
- **A phase never cancels a configured deployment.** `try_fire_pyros()` runs in
  all three descent states. Leaving it out of `CHUTE_DESCENT` meant a rocket
  whose descent merely *looked* main-like -- a big drogue, a light airframe --
  silently skipped the main its config asked for. That is the firmware
  overriding the operator on the strength of an inference.

### DD-024: The Emergency Ladder Has No Bare Descent-Rate Trigger
- **Decision:** The ladder acts only when the drogue has been commanded and the
  rocket has not steadied under a canopy. Free fall alone never triggers it.
- **Why:** a drogue configured below apogee means a deliberate free fall down
  to it, and free fall is fast. A rate trigger would fire the main over the top
  of any such config. An earlier draft did exactly that and broke
  `test_PYR_MODE_02_agl_agl`; `test_FLT_EMRG_02` now guards against its return.
- **The rungs:** 2 s grace, then one retry, then the main early. The retry is
  conditioned on `pyro1_verify_fail` -- the channel never opened, so the charge
  did not light, which is the single failure a second attempt can fix. A
  channel that opened fired its charge and the canopy failed mechanically;
  re-firing an empty channel spends altitude the main still needs. That flag
  had previously been collected and used for nothing but suppressing its own
  re-check.
- **Above 90 m/s the grace is skipped**, since waiting cannot help from there.
  It is set high on purpose: an ordinary failed-drogue descent must reach the
  retry on the grace, not on this.

### DD-025: The Mach Gate Latches On Upward Speed Only
- **Decision:** Apogee is not declared while ascending faster than 100 ft/s,
  nor until the rocket has been slower than that for 1 s. A flight that never
  exceeds it is never gated.
- **Signed, not magnitude.** Testing the magnitude would re-latch on the way
  down, where the rate climbs past the threshold again, and lock apogee
  detection out for the rest of the flight -- a new deadlock in place of the
  one being fixed. Descending fast is not a reason to doubt that apogee
  happened; it is proof that it did.
- **The latch is fed from every ascent sample, not from the gate test.** The
  gate is consulted only once the pyros are armed, and arming already requires
  the rocket to have slowed below 10 m/s. A latch living inside the gate could
  never see a speed above the threshold, so the gate was permanently open on
  exactly the flights it exists for. This was caught by negative-testing --
  breaking the gate deliberately changed no test result, which is what exposed
  that it was never engaging.

### DD-026: Brownout Recovery Needs A Marker, Because The Silicon Cannot Help
- **Decision:** Write a pad marker holding the ground pressure after 10 s of
  PAD_IDLE. On a power-event reset, recover the ground reference from it rather
  than recalibrating -- but only when the barometer shows the board is both
  above the recorded ground and moving.
- **Why a marker at all:** a brownout is electrically a power cycle. The
  RP2040's brown-out detector drives POR, so `HAD_POR` is set exactly as it is
  when somebody connects the battery, and every RAM contents and watchdog
  scratch register is gone. The reset cause can only narrow the question to
  "this was a power event"; the marker and the barometer answer the rest.
- **Why not calibrate:** calibrating defines the current altitude as zero. Do
  that at 600 m and the rocket has no altitude left to deploy against.
- **The motion test is the safety-critical part.** Weather can move the
  pressure by more than the 30 m altitude threshold between the marker being
  written and the board being switched on again. Without requiring measured
  motion, a drifting barometer on the pad would read as airborne -- and the
  descent path arms the pyros. A high-but-still board is reported
  `RECOVER_AMBIGUOUS` and treated as a cold boot: a stationary board does not
  need a parachute.
- **Armed only on the descent path.** A rocket already coming down has passed
  apogee whatever the lost RAM used to think. One still climbing goes through
  the normal arming gate and apogee detection like any other flight.
- **The log restarts.** The flight the log was recording went with the RAM that
  held it, so recovery opens a new log whose T+0 is the moment of recovery.
  That is the only launch time this board can still honestly claim.
- **Two bugs found while building it, both by tests:**
  (a) `BOOT_SENSOR` was a single-tick state, so the verdict was reached on one
  sample -- which is to say on no speed at all. It now lingers, with a 4 s
  deadline so a stuttering sensor cannot hang the boot.
  (b) The level was taken from the newest filtered pressure while the timestamp
  came from an older queued sample, so the two described different instants and
  the speed came out zero. The rate now comes from the queued sample's own
  altitude (whose reference is constant, and cancels in a difference) and the
  level from the marker's ground pressure.

### DD-027: The Flight Log Holds Off Flash Through The Launch Shock
- **Decision:** `hal_log_start()` sets a holdoff; the flush timer does not fire
  until the RAM buffer has filled once. A full buffer and a stop always flush,
  so nothing is dropped to keep the flash quiet.
- **Why:** launch shock -- a battery connector bouncing -- is the likeliest
  cause of the brownout DD-026 exists to survive, and a flash write in progress
  is the worst moment to lose power.
- **LOG_BUF_SIZE went from 512 to 4096, and that is the load-bearing part.**
  At 512 bytes the buffer filled in under a third of a second at the 50 Hz
  default, so "wait until it is full" would have bought about 80 ms over the
  200 ms timer it replaced -- the mechanism would have looked implemented
  without doing anything. At 4 KB it is roughly 2.7 s of flight, which spans
  the shock. Costs 3.5 KB of RAM against about 86 KB free.
- **The trade:** a brownout that never recovers now loses up to a buffer of
  flight data instead of 200 ms. That is the intended direction -- the data is
  worth less than the flight -- but it is a real loss and worth knowing about.
- **Not verified by test.** The holdoff is a timing property of the flash
  service and the host harness does not model flash write windows; it is marked
  as such in TRACEABILITY.md rather than claimed.

