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
- **Superseded by DD-030:** `beep_mode` was never implemented and is removed.
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
  reintroduce exactly what this decision removes. (Removed by DD-030.)

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
- **Amended by DD-028:** the rungs below are replaced; the rule in the title
  stands.
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

### DD-028: The Emergency Ladder Acts On Evidence Of Failure
- **Decision:** The main is brought forward only when, once the most recent
  drogue command has had 2 s to deploy, the rocket descends faster than 35 m/s
  and is not being slowed, for 1 s. The drogue retry keeps its own evidence,
  `pyro1_verify_fail`. Nothing acts on the absence of a settled descent.
- **Why:** a drogue opened at apogee starts from zero and is still accelerating
  toward its terminal rate for seconds. "Not settled within 2 s" is therefore
  true of a working drogue, and a ladder keyed on it forced the main 2 s after
  the drogue on every flight that set the main by altitude -- 541 m, 1100 m and
  3884 m instead of 152 m in the review's measurements. FLT-EMRG-01's old text
  mandated exactly that, so it was rewritten with FLT-EMRG-03 as the guard.
- **"Not being slowed"** is a fall of more than the descent tolerance (a
  quarter of the rate) since the window opened; a fall restarts the window. A
  drogue fired into a fast descent is still decelerating when its grace ends
  and never trips it; a shredded one at a steady terminal rate does.
- **The cost:** on the H73 profile a failed drogue now brings the main out 4.6 s
  after apogee at 1071 m, against 2 s before. The operator narrative expects
  about 3 s. Shortening the grace or the hold buys time on a failure at the
  price of forcing mains under slow drogues.
- **A refused drogue counts as commanded.** A board that could not fire the
  drogue has no drogue, and the main rung applies.

### DD-029: AGL And FALLEN Triggers Are Corrected For The Filter Lag
- **Decision:** `should_fire_pyro()` compares `altitude + speed x 500 ms` (on
  the way down) rather than the filtered altitude.
- **Why:** the 500 ms IIR makes the filtered altitude trail a descending rocket
  by rate x tau -- 56 m at a ballistic 100 m/s, turning a 400 ft drogue into a
  220 ft one. Closed-loop, every AGL channel now fires within 8 m of its
  setting (121 m for 122 m ballistic, 62 m for 61 m under drogue).
- **Not applied** to SPEED (the speed lags too, but a speed trigger is not a
  height) or DELAY.

### DD-030: Every Configuration Key Is Read By Something
- **Decision:** `beep_mode`, `max_coast_s`, `log_enabled` and `buzzer_startup`
  are removed; `telem_rate_hz` now sets the in-flight telemetry cadence and
  `log_rate_hz` thins the logged samples (never the events).
- **Why:** a key in `config.ini` is a promise that changing it changes the
  board. `beep_mode` was a visible UI control with no effect. The two rates are
  kept because CFG-SUBSYS-01 asks for configurable telemetry and logging; the
  other four would each have contradicted a requirement if wired
  (`log_enabled` SYS-DATA-01, `buzzer_startup` SYS-STATUS-01 and BUZ-CODE-08,
  `max_coast_s` DD-022). Old files carrying the keys still parse (CFG-08) and
  lose them on the next save.

### DD-031: A Faulted Board Sends No State Sentence
- **Decision:** No `$PYRO` in boot states or FAULT; a `!FAULT <names>` line
  every 5 s instead.
- **Why:** the ground-station contract has six states and no FAULT, and state 0
  means "on the pad, ready". A board that cannot fly was reporting itself ready
  at 1 Hz.

### DD-032: A Refused Fire Is Not A Fire
- **Decision:** `hal_pyro_is_firing()` read straight after `hal_pyro_fire()` is
  the board's acknowledgement. A channel is recorded as fired only if it reads
  true; otherwise the command is logged as `PYROn_REFUSED` and not repeated.
- **Why:** MK1C cannot fire yet and the flight log, CSV, telemetry and
  `/api/status` all said it had. No HAL function was added: the semantics were
  already true of MK1A, MK1B, the test HAL and the simulator, and are now
  written down in `hal.h` and `pyro.h` for the MK1C firing sequence to honour.
- **The same function, `flight_pyro_energise()`,** is the only fire site for the
  flight and the ground test, so PYR-DEPLOY-02's interlock sits below both.

### DD-033: The Pad Marker Is Written Inside The Flash Window
- **Decision:** `flight_flash_service()`, called by the main loop inside the
  flash window, writes the pad marker; the PAD_IDLE detector does not.
- **Why:** the detector runs at STAGE 3 with the window shut, so
  `hal_fs_write_file()` refused the write every time and brownout recovery
  (DD-026) could never engage. Found on the bench: no `pad.mkr` on any board.
  The host HAL has no window, which is why every test passed.

### DD-034: No State-Changing Request While Flying
- **Decision:** From launch to landing every POST, and the bench capture, is
  refused with 409. GETs are answered.
- **Why:** `/api/config`, `/api/pins` and `/api/beeps/play` already refused, but
  `/api/reboot`, `/api/ota`, `/www/`, `/api/serial`, `/api/beeps` and
  `/api/lua/*` did not -- a browser could reboot a flying board.
- **The cost:** a board stuck in a flight state (a false launch that never arms,
  or a sensor that dies in flight, DD-022) can no longer be rebooted or updated
  from the browser; it needs a power cycle. The firmware cannot tell such a
  board from one that is flying, which is the point of the interlock.

### DD-035: The Flight Log Is Published Every Second, Inside The Flash Window
- **Decision:** `log_flash_service()` calls `lfs_file_sync()` on the flight log
  once a second while it has unsynced bytes, in a window where no flush write
  happened.
- **The rule it lives under:** every flash write -- flush, sync, the partial
  block a sync makes the next write copy -- runs between core1's work units,
  in the window core0 opens at STAGE 7 once core1's grant has expired. Core1 is
  then idle in RAM, so no XIP fetch can coincide with a program or erase.
  `flash_refusals` stays 0 if that holds: the littlefs driver refuses and
  counts any program or erase outside the window.
- **Why:** littlefs publishes what a file holds only on sync or close, and the
  log closed only at LANDED. On MK1C a 60 s bench log read 0 bytes until its
  close; a reset 30 s in, with no close, now leaves every row up to 0.5 s
  before the reset.
- **The cost, measured on MK1C with Lua running, 60 s at 50 Hz:** erases 20 to
  81, `stage_max_us[7]` 46 to 73 ms, loop overruns 20 to 77, Lua ticks down
  about 4 %, no dropped sample. Each sync makes the next write copy the partial
  last block. `LOG_SYNC_MS` trades that stall rate against how much flight a
  power loss can take with it.

### DD-036: An MS5607 Conversion Is Timed From Its Command, And Impossible Readings Are Refused
- **Decision:** The D1 and D2 reads wait until 9.1 ms after the conversion
  command, measured in microseconds from the command itself (the datasheet
  maximum at OSR 4096 is 9.04 ms). A reading of zero, or a pressure outside the
  sensor's 1-120 kPa range, is not fed to the filter. Both events are counted on
  `/api/status` as `pres_waits` and `pres_rejects`.
- **Why:** the task's millisecond deadline was taken from the top of the loop,
  before STAGE 1's USB and lwIP work, so a long STAGE 1 ate the ~1 ms margin
  and the read came back 0 -- which compensates to a large negative pressure,
  hundreds of metres of altitude through the IIR. Reproduced on MK1C under HTTP
  load: a bench false launch (LAUNCH row at a filtered 95 290 Pa), and 18
  impossible readings in 60 s. With the fix, 0 in 120 s of heavier load, and no
  deferral at all when the board is idle.
- **Why the gate as well as the timing:** one bad sample on the pad is a false
  launch, and in flight it is a false AGL trigger. The gate costs nothing inside
  the sensor's range, so it cannot reject a real reading anywhere a rocket can
  go.
- **The bench false launches** on MK1B and MK1C that led to the 100 ft trigger
  (48d47c0) were attributed to weather drift. MK1B's recorded maximum was 243 m,
  which drift does not produce and this does.

### DD-037: A PC On USB Is Detected From Start-Of-Frame, And A Charger Cannot Be Detected
- **Decision:** A USB host is attached while the SIE's last start-of-frame
  number (`usb_hw->sof_rd`) has changed within the last 100 ms. The main loop
  asks before each `dispatch_state()` and passes the answer to
  `flight_set_usb_attached()`. While attached, the board detects no launch,
  says nothing but one double chirp on attach, writes no pad marker and takes
  no brownout recovery (USB-01..05).
- **Why SOF:** a host sends a start-of-frame every millisecond while it is
  awake, and nothing else does. So a false "attached" needs a real host, and
  every other error means "not attached", which leaves launch detection on
  (USB-07). A false "attached" in flight would mean no deployment.
- **Rejected:**
  - *VBUS.* On MK1A, MK1B and MK1C, USB VBUS goes only to the TP4057 charger
    (VCC, its capacitor, the two LED resistors and a test point). No RP2040
    pin sees it, and TinyUSB forces the SIE's VBUS-detect override anyway.
  - *`tud_ready()` / suspend.* An unplugged cable is reported as a suspend
    only if the idle line reads as J. D- floats with no host, so that is not
    assured, and the error would be "attached" on a battery.
  - *Data-line state for chargers.* A BC1.2 charger shorts D+ to D-, which
    reads as SE1 through the device's pull-up. A floating D- with no cable
    can read the same, so this can false-trigger in flight. Not shipped.
- **What it cannot do:** see a charger, a sleeping PC or a suspended bus. All
  three read as detached. The board then announces and detects launches as if
  on battery, and chirps again when the host wakes.
- **To detect a charger** needs a board change:
  - A VBUS divider to a spare GPIO. MK1C has GPIO2-5, 9, 10 and 13-15 free.
    GPIO24 is unconnected on MK1A/B. RP2040 pins are not 5 V tolerant.
  - Or the TP4057's open-drain ~CHRG and ~STDBY to two GPIOs: either one low
    means the charger has input power. Those nets also drive the LEDs, and
    with VBUS present an idle one sits near VBUS minus the LED drop, so check
    that voltage against the pin's limit.
  Either signal would be ORed with SOF in `usb_host_active()`.

### DD-038: Test Mode Flies On USB, And Lives Only In RAM
- **Decision:** `POST /api/test_mode/on` and `/off` set a RAM flag. While it
  is on, a board on USB behaves exactly as on battery: it detects a launch,
  fires, writes the pad marker and announces. Every boot starts with it off.
  It cannot change from launch to landing: the HTTP interlock refuses the
  request, and the flight layer ignores it anyway. The web UI asks for
  confirmation before turning it on, and shows a warning while it is on.
- **Why:** a chamber flight watched in the web UI, and a bench soak for false
  launches, both need the flight machine with a PC on the port (USB-01 turns
  it off). A PC on USB means a bench, never a flight, so this is as safe as
  flying on battery. The igniters are the operator's business, and the
  prompt says so.
- **Why RAM:** nobody should plug a board in and find it still in test mode
  from a session someone forgot about. With the flag in a file, whether
  "USB means grounded" held would depend on that file.
- **The cost:** a reboot ends test mode, including one mid-test. For the same
  reason, brownout recovery on USB can never run in test mode: the reset
  clears the flag before the recovery verdict. Recovery has to be tested on
  battery.
- **On and off go in the path, not a body:** a body can arrive in a later TCP
  segment, and gathering it holds the flash window, which this request has no
  use for. Found on hardware: Python's urllib sends the body separately, and
  a first-segment-only handler read it as empty.
- **Chirp:** switching test mode off while attached is an attach, so it
  chirps. Switching it on resumes the pad announcement, which confirms it by
  ear.

### DD-046: Every Sample Carries The Time Of Its Reading
- **Decision:** the HAL stamps each reading from the hardware timer, to the
  microsecond, at the moment it describes (SNS-PRES-08):
  - the MS5607 at the middle of its D1 conversion, from a time saved when D1
    is commanded. `conv_start_us` could not serve, because D2's command
    overwrites it;
  - the BMP280 half a conversion before the read.

  The pressure layer takes the stamp (`pp_feed_us()`) and carries the
  microseconds to its samples beside the milliseconds. Every detector hold
  and dwell that measures the sensor, and every logged sample row, uses the
  sample's time (FLT-RATE-05, DAT-02). The PAD_IDLE gate that compared the
  loop's time with a sample's is gone.
- **Why:** a reading was stamped with the loop's millisecond at the top of the
  iteration that read it: 16 ms after the conversion normally, and later by
  the whole of any flash stall in between. Every dt inherited it. Through T0's
  stall model the worst speed error at 100 m/s went from 6.1 to 8.3 m/s; now
  6.5. The holds on the loop clock let the loop's lateness choose which
  sample decided the launch.
- **The BMP280 stays in normal mode.** Forced mode, as planned, would change
  the driver in `boards/mk1a/` and `boards/mk1b/`, which cannot be checked
  without the bench boards. A free-running BMP280 keeps converting through a
  stall, so what is read is never more than one conversion (13.8 ms) old.
  Stamped at half of that before the read, it is within ±7 ms whatever the
  loop did.
- **What the host can test:** `hal_common.c` runs only on the RP2040. The
  stamping arithmetic is a tested helper, and the test HAL models the
  stamping, stalls included, so everything downstream is tested under it.
  `/api/status` shows `sample_interval_us` and `stamp_lag_max_us` for the
  bench check.
- **Timer-driven sampling rejected:** a hardware alarm starting conversions
  would not keep sampling uniform through a stall, because a flash erase runs
  with interrupts off. True stamps make uniform sampling unnecessary.

### DD-045: The Ground Reference Re-Seeds After A Step
- **Decision:** When every sample has been rejected by GND-CAL-03's 50 Pa gate
  for 5 s, and the board is still (under 1 m/s), the reference restarts from
  the current filtered pressure (GND-CAL-06). `!GND reseed` goes out on
  telemetry, `ground_reseeds` counts it on /api/status, and the pad marker's
  dwell restarts so the marker records the new ground.
- **Why:** a board powered at the prep table and carried to a higher or lower
  pad saw every later sample rejected, and its reference froze at the old
  ground for good (N9). Since T4 the filter moves through a small step
  smoothly enough for the mean to creep after it, so a 60 Pa step now
  recovers without help. From 100 Pa up, the mean still locked out. It now
  re-seeds within 5 s, in both directions.
- **5 s, not the pressure-filter prompt's 30 s:** for 30 s after the rocket
  is set down, the reference, and every altitude, would be wrong. No gust
  lasts 5 s (`test_T6_gusts_never_reseed`), and drift stays inside the gate
  (`test_T6_drift`).
- **Still, because** a board being carried is still moving from one ground to
  the next; a re-seed mid-walk would only need another.

### DD-044: The Filter Keeps Fractions, And T+0 Comes From The Reading
- **Decision:** The pressure filter's state is Q8 fixed point, with its step
  computed exactly (SNS-PRES-02), and SNS-PRES-04's forced 1 Pa step is gone.
  Heights come from the fractional pressure. T+0 is read from the median of
  three's own reading, unfiltered (FLT-LAUNCH-03).
- **Why the filter:** at 20 ms its step is 3.8 % of the difference, which in
  whole pascals rounds to nothing under 26 Pa. The forced 1 Pa step that
  broke the stall made it a rate limiter that passed the noise through. On
  the pad, 1.2 Pa of sensor noise left 0.74 Pa in the filter and 2.6 m/s of
  speed noise. Every speed stepped by 4 m/s, a whole pascal in 20 ms. Now:
  0.17 Pa, 0.22 m/s, and touchdown to LANDED in 1.6 s at any landing height,
  where before it never landed by stillness.
- **Why T+0 moved:** a quiet filter lags the first half metre by its time
  constant; at 2 g, T+0 came 260 ms after the truth. The old filter was only
  40-100 ms late, and by accident: its forced steps reacted to the first
  pascals early. The median's reading carries its own time and 7 cm of noise
  against a 50 cm threshold, so T+0 is now within one sample of the truth.
- **Rejected:** float state. Q8 needs no FPU work in the per-sample path, and
  101 325 Pa x 256 fits an int32.

### DD-043: The Launch's Ground Pressure Comes From Before The Rise
- **Decision:** At launch the reference freezes to the mean of the blocks of
  GND-CAL-01's 5 s mean that ended before T+0, the first sample of the rise
  (GND-CAL-04). Each block records when it began. With less than a second of
  such blocks the reference is flagged degraded (GND-CAL-07), and with none
  it is kept as it is.
- **Why:** the reference froze at detection, 1-2 s into the climb. The first
  metres of that climb passed GND-CAL-03's 50 Pa gate, so every AGL value read
  0.17 m (30 g) to 0.42 m (2 g) low. It now reads 0.08 m, about 1 Pa, which
  is the HAL truncating each reading to whole pascals.
- **T+0 is already known.** FLT-LAUNCH-03 records the first sample above
  50 cm, so no fixed look-back (the pressure-filter prompt's 3 s) is needed,
  and a fast launch does not lose three seconds of pad.
- **Degraded, not refused:** a rocket launched the moment the board reached
  PAD_IDLE still flies; its AGL values are only as good as the calibration.

### DD-042: Triggers Hold For A Duration, And Speed Passes The Clamps
- **Decision:** Launch needs its height and speed condition held for 100 ms
  of sample time (FLT-LAUNCH-07); apogee needs speed at or below zero held for
  60 ms (FLT-APO-01). Durations, never sample counts. Every detector takes its
  speed from an unclamped height, carried beside the clamped altitude in the
  pressure layer's ring (SNS-ALT-04); the clamps now apply only to what is
  reported.
- **Why the holds:** the median of three (DD-040) stops one bad reading, and
  two in a row still reached detectors that fired on one sample: a launch on
  the pad, an early apogee in coast.
- **Why the height:** T3's coast test still failed with the holds in. Two
  high readings drove the filtered altitude 60 m down, below the pad; the zero
  clamp held it at exactly 0 while the filter decayed back, and a constant
  altitude is a speed of zero for 180 ms, longer than any hold. The same
  clamp at 8000 m read as zero speed on the way up (N26): a flight to 9.3 km
  declared apogee 14.9 s early.
- **Cost:** launch is detected up to 100 ms later, with T+0 unchanged.
  Apogee moved from +0.59 s to +0.65 s after the true one.
- **Consequence:** on the pad's own level the zero clamp had been hiding the
  sensor's noise from the landing test, which is why touchdown there took
  1.9 s. Unclamped, the noise reaches it, and landing waits for the 60 s
  timeout on the pad's level as it already did anywhere else, until T4
  quietens the filter.
- **Rejected:** counting samples. At T9's 90 Hz a count of five would be a
  55 ms hold; `test_T3_durations_not_counts` fails that.

### DD-041: Brownout Recovery Reads The History From Power-On
- **Decision:** The pressure layer keeps the median's output from power-on in
  every state (`pp_history_*`). Recovery takes its level as the median of the
  newest 250 ms, and its speed as that level against the median of a window
  ending 350 ms earlier (FLT-BROWN-02). A flight it rejoins starts the
  pressure layer against the marker's ground (`pp_resume_flight()`) and reads
  continuity first (FLT-BROWN-06). The marker is invalidated at LANDED
  (FLT-BROWN-04), and /api/status says why a boot was cold (FLT-BROWN-05).
- **Why:** recovery asked for samples before the pressure layer produced any,
  so it never engaged on the hardware (N23). Making it engage exposed three
  more gaps, each hidden by tests that primed the layer:
  - A rejoined flight got no altitude, because nothing started the pressure
    layer. It would have deployed nothing.
  - It skipped BOOT_CONTINUITY, so both channels read as open and
    PYR-SAFE-01 refused them. It would have deployed nothing.
  - The marker is never deleted, and every battery connection is a power
    event, so every power-up ran recovery against the last session's marker
    (N25). The operator narrative powers up twice per flight, charges
    connected.
- **Medians, not a line fit.** The history is only the median of three, so
  two bad readings in a row pass it. On the pad, one bad reading inside the
  level's window reads as 80 m up and climbing; placed right, it reads as
  falling, and a recovered descent arms the pyros at once. A median of each
  window moves by one rank for each bad reading. The speed's noise is about
  0.15 m/s; a two-reading speed's was 1.7 m/s, too close to the 5 m/s
  threshold. `test_T1_glitch_on_the_pad` puts one and two glitches at every
  position of the history.
- **Invalidated, not deleted:** `hal.h` has no delete, and a new board must
  not have to implement one. A zeroed marker fails `pad_marker_valid()`.
- **Rejected:** a least-squares slope with outliers discarded after a first
  fit. The first fit is what the outliers corrupt.

### DD-040: A Median Of Three Before The Filter
- **Decision:** The pressure layer passes the median of the newest three
  readings to the filter, stamped with the middle reading's time
  (SNS-PRES-07). Calibration takes the median of its 10 readings instead of
  their mean (FLT-BOOT-08).
- **Why:** a single reading inside the sensor's range but 11 kPa or more low
  declared a launch (N24), and one high reading in the last second of coast
  declared apogee up to 0.4 s early on a flight too slow to latch the Mach
  gate. A flipped high bit in the raw value is enough. One glitch during
  calibration left the ground reference 2 kPa off, and GND-CAL-03's 50 Pa
  gate then rejected every sample after it, so it stayed off.
- **Stamped at the middle reading.** On a monotonic signal the median is the
  middle reading exactly, so with its own time the stage costs one sample of
  latency and moves no altitude. Stamped with the newest reading's time,
  every altitude would be reported 20 ms early. Measured in the closed-loop
  suite: launch, apogee and the first deployment each move +20 ms in all 36
  flights, altitudes at most 2 m.
- **Rejected:** a median of five, which also stops two bad readings in a row
  but costs 40 ms everywhere; T3's held triggers stop those instead. A
  rate-of-change gate, whose limit would have to be tuned to the fastest real
  rocket. A Hampel filter, which needs a noise estimate the pad has not
  measured yet.
- **After priming.** `pp_test_prime()` starts the window empty, and until it
  holds three readings the newest goes straight through. No time is emitted
  twice. On the hardware calibration always fills the window first.

### DD-039: HTTP Is A Byte Stream, Between Two Rings
- **Decision:** Each connection owns an rx ring and a tx ring (`net_ring.c`)
  and an HTTP engine (`http_conn.c`). The engine parses the request from rx a
  line at a time, takes the body at the pace its consumer allows, and writes
  the response into tx. The lwIP adapter in `http_server.c` does four things:
  - queue what arrives;
  - copy it into rx as there is room;
  - call `tcp_recved()` for what the parser consumed;
  - feed tx to `tcp_write()` as the send buffer allows.

  The callbacks only queue. The work runs in `http_server_service()`, from
  `net_service()` in the main loop.
- **Why:** the old server parsed each segment as it came. Several failures
  followed from that:
  - A body in a later segment was read as empty. The Beep Codes audition
    answered 400 to a valid request (N22), and test mode had to move on/off
    into the path to avoid it.
  - A header block split across segments was parsed as a malformed request,
    and so was a request whose first segment was a single byte.
  - A body segment arriving in its own read was answered as a fresh request.
    An upload sent in small writes got a 400 for every segment.
  - Waiting on the flash window depended on lwIP holding a refused segment
    and redelivering it on its 250 ms timer.

  Against the old server, `support/http_stream_check.py` passes 4 of 16.
  Against the new one it passes 16 of 16 on all three boards.
- **What it also fixed:**
  - N8: the OTA reply arrives. The reboot runs through `pending_reset` once
    the reply is in lwIP's hands, instead of `pfb_perform_update()` spinning
    inside the callback.
  - OTA answers `Expect: 100-continue`, so curl no longer waits a second
    before sending. An OTA now takes 2.9 s on MK1C (was 37 s) and 5.6 s on
    MK1A (was 81 s).
  - Every response carries Content-Length, including files.
  - An upload that dies mid-body is never closed, so littlefs keeps the
    previous file whole instead of committing a truncated one.
- **Why rings:** smallest_tcp (github.com/n9wxu/smallest_tcp), the stack
  intended to replace lwIP here, gives each connection application-owned RX
  and TX buffers behind a vtable. It advertises the RX buffer's free space as
  the window, and the application drains RX and fills TX from its main loop.
  `net_ring.h` follows those operations. Replacing lwIP means replacing the
  adapter; the HTTP engine and routes stay as they are.
- **The cost:** 4 exchanges of 2 kB rx, 2 kB tx and a 5 kB work buffer. The
  work buffer is shared, one use at a time, by the gathered body, the part
  of a response that does not fit tx, and the littlefs cache of a streamed
  file. That is about 38 kB in place of about 32 kB of static buffers the
  old server kept; bss grew 8.6 kB. A pcb gets an exchange only when it
  first sends a byte, so a browser's speculative idle sockets cost nothing.
  While all four are busy, a new request waits in its pbufs, held back by
  its TCP window.

