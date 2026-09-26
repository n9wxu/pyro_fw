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
- **Decision:** Pyro arming requires max vertical speed during ASCENT exceeded 10 m/s.
- **Amended by DD-050:** "slowed below it" is all: arming descending counts,
  so a failed sensor near apogee cannot close the window for good. And not on
  a suspect fit.
- **Amended by DD-049:** and not before p < 0.9965·p0, about 30 m climbed.
- **Amended by DD-048:** 10 m/s of true speed, the fit's. This read 20 m/s
  while the speed was the filter's, on the assumption that the filter halved
  it. A rocket still climbing at 10 m/s at the launch detector's 100 ft
  reaches 35 m.
- **Rationale:** After a false launch (from drift), speed is ~0 m/s. Without this
  gate, arming happens immediately (speed < 10 m/s), then apogee fires (speed ≈ 0).
  This gate blocks the entire false-launch → ground-fire chain.

### DD-015: Landing Timeout for Elevation Mismatch
- **Decision:** Detect landing if descent has lasted 60 seconds and speed is below
  5 m/s, regardless of AGL altitude.
- **Amended (N7, C8):** the timeout needs stillness -- under 2 m/s for 1 s, on
  a sensor that has not failed -- not "below 5 m/s". A main descends at
  3-6 m/s, and any flight whose main was still out 60 s after apogee was
  declared LANDED in the air, its log closed with the descent unrecorded. A
  5 m/s main from 1.3 km now lands 1.7 s after touchdown.
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
- **Superseded by DD-049:** the Mach lockout replaced the gate.
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
- **Superseded by DD-048:** AGL and FALLEN compare the fit's height, which
  does not lag, so the lead is gone.
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
- **Amended by DD-051:** the read is a one-shot alarm's, still 9.1 ms after
  the command, and `pres_waits` counts loops that found the conversion still
  in flight.
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

### DD-055: MK1C's Firmware Checks Presence And Shorts, Nothing Else
- **Decision:** at the user's direction -- "The ONLY FW checks we need are
  those that verify the pyro is present and we do not have short circuits.
  We do not need live HW checks." -- MK1C's pyro backend runs the T2
  tracking test and the two short latches, and no other probe:
  - presence: a channel following the biased bus (DD-054);
  - a bus that will not rise under its bias: shorted to ground, by the bus,
    a harness lead, or a shorted low-side FET behind a fitted match;
  - the bus at the pack with nothing armed: a shorted high side.
- **Removed:** the T3 channel-bias probe, the bus decay probe, the bias-hold
  bench mode, the waveform capture and its `/api/capture` endpoint (with the
  board interface's capture calls, the status fields `bias_a`, `bias_b`,
  `decay_tau_us` and `wave_state`, and `support/pyro_check.py`, which
  needed it), and the arm interlock that guarded the capture's arm mode.
  The capture held the tree's last seven sleeps, so DD-053's ratchet is
  empty.
- **Kept: the arm pump.** The firing bus is energised only while ARM_TOGGLE
  pumps U9's enable, so firing needs it. Its start and stop live in
  `boards/mk1c/arm_pump.c`, uncalled: MK1C's firing path is not built
  (`pyro_fire()` refuses), and is task F1.
- **A shorted low-side FET without a match** is no longer reported; T3 found
  it. It cannot fire anything, and with a match fitted the tracking test
  sees the bus pulled down.
- `board_pyro_mk1c_tests` runs the real backend against the plant: a fitted
  match reads present and an absent one open, a shorted bus, a shorted low
  side behind a match and a shorted high side each latch, and the only
  stimulus is the tracking pulse, once in 500 ms, with nothing holding the
  loop.

### DD-054: MK1C's Firing Bus Is Designed Around The Board As Measured
- **Decision:** at the user's direction, the firmware and its model follow
  the bench MK1C, not DESIGN.md 4's algebra. Scoped at CN1 and read by the
  board's ADC on 2026-09-26: R120 330 ohm (metered), R103 and C115 as
  designed, but U9's OUT, off, conducts back into the part above about
  0.72 V, a junction with about 250 ohm behind it, 2.4 mA at 1.5 V. The
  user: "an internal reverse bias is to be expected". So under bias the bus
  sits at about 688 counts (1.66 V), not 1058, drawing about 4 mA from
  GPIO23 at its 4 mA default drive; the channels under their own bias read
  about 1262, not 1214, because their bias diodes drop less at 0.2 mA.
- **Presence is a ratio** (`boards/mk1c/pyro_sense.h`): a channel is
  present when it reads at least half the bus in the same tracking test,
  and the test says nothing when the bus did not rise (under 200 counts).
  DESIGN.md S3 already says presence "does not use absolute levels"; the
  absolute 400 counts it replaces kept 1.7x of margin on this bus, which
  moves with U9 and its temperature, where the ratio keeps 2x whatever the
  bus does. `pyro_get()` and the arm interlock use it.
- **The model is the board** (`sim/plant/plant_mk1c.c`): U9's reverse path
  and the GPIO-and-Schottky bias sources, fitted to the bench, and the
  bus's 1.1 uF. It reproduces the ADC's 685-690 on the bus, 1258-1266 on
  the channels, and the scope's fall: 415 us to 0.9 V, 2.32 ms from 0.6 to
  0.2 V, where DESIGN.md's linear bus took 2 ms to reach 0.9 V.
  `plant_tests` holds it there.
- **What it costs:** an open R_BLEED no longer shows in the bus level --
  U9's path carries the bus either way, 685 against 730 counts -- where on
  paper it moved 1058 to 1214. Below the knee the bus decays through the
  pull-down alone, so the decay there still shows it (about 2.3 ms against
  16 ms); nothing checks that yet. The tracking current falls to about
  0.11 mA, safer than the 0.2 mA designed.
- **The bench script** graded the board against this characterisation for
  a while, finding an open R_BLEED from the decay below the knee. It went
  with the waveform capture it depended on (DD-055).

### DD-053: No Sleeps: The Exec Loop Is The Only Clock
- **Decision:** at the user's direction, no code sleeps or busy-waits
  (PWR-WAIT-01). The exec loop sets the time; anything that has to wait parks
  on a deadline that a later iteration checks, and the only interruptions to
  the loop are flash writes. `support/wait_check.py` fails CI on any sleep,
  busy-wait, or blocking DMA or PIO wait in `src/` or `boards/`, with a
  ratchet of the sites not yet converted that only shrinks.
- **Why:** a sleep inside an iteration is time the loop does not have. The
  first flash of the one-shot, on a second MK1B, showed it: the continuity
  check held PYRO_COMMON_EN for a `sleep_ms(10)` settle once a second, and the
  loop overran once a second (250 in 252 s). The bench MK1B had never shown
  it, because both its channels are released to Lua, which skips the check.
- **MK1B's continuity** now works like MK1A's: the settle is a deadline, the
  reading comes on a later `pyro_update()`, once a second, and a fresh one
  lands a settle after a pulse. The first `pyro_update()` starts the cycle,
  not `pyro_init()`, because the release claim comes after init and a board
  with both channels released never calls `pyro_update()`: its common, then
  Lua's pad, is never left raised. `board_pyro_tests` runs the board file on
  the host against `test/fake_sdk`, whose sleeps fail the test.
- **The sensor's bring-up** is steps the loop takes too: bus recovery at one
  SCL edge a loop (I2C has no lowest clock rate), with a STOP on every SDA the
  board has; the pull-ups' settle; the MS5607's 2.8 ms PROM reload and the
  BMP280's 2 ms start-up, each a deadline. It takes about a quarter of a
  second, inside BOOT_SETTLE. `hal_pressure_init()` starts it on the pressure
  task's slot, which samples once it knows the sensor; BOOT_SENSOR waits for
  it (`hal_pressure_sensor()` is -1 until then), and one still going
  `SENSOR_BRINGUP_MS` after boot is a missing sensor. `sensor_bringup_tests`
  runs each board's own `pressure_board.c` against fake sensors that refuse
  a transfer during their reset. On MK1B the recovery's STOP used to reach
  only the BMP280's pad, never the MS5607 it carries. The unused synchronous
  `ms5607_read()` and `pressure_sensor_read()` are gone.
- **Left:** MK1C's bench waveform capture, 7 waits in the flash window on a
  bench request. The arm pump's pacing is part of the arm interlock's safety
  argument, so its conversion waits on the user. In the SDK, `uart_init()`
  can busy-wait only when re-initialising an enabled UART, which ours never
  is, and `stdio_get_until()` is reachable only from newlib's `_read()`,
  which nothing calls.
- **Outside the check:** waits on a bus or a peripheral's handshake, which
  wait for hardware rather than for time: the I2C transfers, the MS5607
  one-shot's wait for STOP inside its handler (at most 0.15 ms at 400 kHz),
  and core1's power-state acknowledgement.

### DD-052: Each Board Runs Its Sensor Bus As Fast As Its Device And Its PCB Allow
- **Decision:** at the user's direction, the I2C speeds are part of each
  board support package: `BOARD_MS5607_I2C_HZ` and `BOARD_BMP280_I2C_HZ` in
  `boards/<name>/board_pins.h`. Each is the device's fastest, or slower where
  that board's PCB cannot carry it; a reliable connection comes first. The
  device limits stay with the drivers (`MS5607_I2C_MAX_HZ`,
  `BMP280_I2C_MAX_HZ`), and `pressure_board.c` fails the build if a board
  asks for more.
- **The devices** (docs/datasheets/): the MS5607's I2C clock is 400 kHz at
  most (page 5). The BMP280 lists standard, fast and high-speed modes (pages
  27 and 31), not fast-mode plus; high-speed runs to 3.4 MHz, but the RP2040
  has no high-speed mode (section 4.3.2). Both are 400 kHz, fast mode.
- **The boards,** read from their design files: fast mode allows a 300 ns
  rise (UM10204 page 44), which 4k7 holds to about 75 pF of bus (page 50),
  several times what one sensor and a short trace present.
  - MK1A: the BMP280 on GPIO20/21 with R1 and R2, 4k7 (its schematic PDF):
    400 kHz.
  - MK1B: the MS5607 on GPIO10 with GPIO7's SCL, R11 and R10, 4k7 (its KiCad
    board): 400 kHz. The BMP280's SDA, GPIO6, has no pull-up but the
    RP2040's own 50-80k, too slow an edge for fast mode; no BMP280 is fitted
    on this board, but the probe there runs first, at 100 kHz, and the rate
    goes up before the MS5607's probe, which then exercises it.
  - MK1C: the MS5607 on GPIO6/7 with R3 and R5, 4k7 (its KiCad board):
    400 kHz.
  - The reference template: 100 kHz until a new board's pull-ups are read.
- **Why faster:** the one-shot's budget (DD-051). At 100 kHz a conversion
  started at the top of a loop was ready 9.87 ms into its 10; at 400 kHz,
  9.29 ms. `ms5607_tests` runs at each MS5607 board's own rate and asks for
  0.5 ms to spare. The handler's time on core0 falls from about 0.8 ms a loop
  to 0.2 ms.
- **Owed on the bench:** at the new rate on each board, `pres_rejects` 0, no
  bus errors on the telemetry, and `sample_interval_us` without `pres_waits`.

### DD-051: The MS5607 Converts Once A Loop, Read By A One-Shot In RAM
- **Decision:** each loop takes the conversion the last loop started and
  starts the next. From the start to the take, an interrupt state machine
  owns the sensor: the loop forces the alarm's interrupt, whose handler sends
  the command, stamps it from the hardware timer and arms the alarm 9.1 ms on
  (DD-036's margin); at the alarm it reads the ADC and leaves the result, with
  its stamp, for the next loop. Temperature is converted once in ten; each pressure is
  compensated with the temperature at its own time, on the least-squares line
  through the last four temperature readings, carried no more than 200 ms past
  the newest (SNS-PRES-12). At the 10 ms loop that is 90 pressures a second,
  each stamped at the middle of its conversion (SNS-PRES-08). The pressure
  task runs first in the loop, before STAGE 1's USB and lwIP work, so the
  command sits at a steady offset from the period.
- **Why:** the loop clocked a three-phase state machine, one phase an
  iteration: D1, then D2, then back to idle, 30 ms a sample and more when a
  read came early. Measured on MK1B and MK1C, a new pressure every 27-35 ms,
  where the code, the docs and the host tests assumed 20. The loop now only
  schedules, and a conversion never waits on the loop's work.
- **Why the handler stamps it:** the user's rule: the driver provides the
  stamp, the time the pressure was measured, and the MS5607's must come from
  the interrupt state machine, which runs apart from the loop. The stamp is
  the conversion's middle, from the handler's own clock as the command ends,
  not the time of the read: an erase holds interrupts off, and so the read,
  for up to tens of milliseconds, but not the conversion. A late loop or a
  held read moves no stamp (`test_SNS_PRES_08_held_read_keeps_its_stamp`,
  `test_SNS_PRES_08_late_loop_keeps_the_stamp`). The temperature needs no
  such care: the sensor's thermal mass keeps it slow.
- **Why a line through the temperature:** a temperature reused as read is up
  to ten conversions old. For a sensor warming at 1 °C/s that costs 12.1 Pa
  RMS; carried along its line, 0.71 Pa RMS and 2.00 Pa at worst
  (`test_T9_temperature_reuse`), under the datasheet's 2.4 Pa of noise at OSR
  4096.
- **Why the handler is in RAM:** it can come due while core0 is erasing
  flash. Interrupts are held off across every erase and program, so it waits
  for the erase, and from RAM it could not fault even were that ever not so.
  The SDK's i2c and time functions are in flash, so the handler drives i2c1's
  registers and reads the timer itself, through `ms5607_bus.h`, whose calls
  are forced inline. `support/prove_core0.py` fails the build if the handler,
  anything it branches to, through a veneer or not, or any address it loads
  is in flash; a `time_us_64()` planted in the handler fails it.
- **Start, then work:** `ms5607_async_cycle()` takes the finished conversion
  and starts the next before the loop works on the one taken. The first
  flash, on a second MK1B (2.1.680), started it after the compensation,
  filter and fit, up to 2.7 ms: the next conversion then missed the next loop
  every other loop, 47 % of loops waited and the rate was 50 Hz. The fake bus
  reproduces that (47.3 %) with 2.7 ms of work a pressure, and the cycle
  makes it none (`test_ms5607_work_costs_no_samples`). The same board read
  its MS5607 at 400 kHz with no rejected reading.
- **What the host can test:** the state machine is `ms5607_oneshot.c`, and
  `ms5607_tests` builds it against a fake bus and clock (`test/ms5607_bus.h`):
  the stamp, a read held 60 ms, a loop 50 ms late, the 9.04 ms wait, one
  conversion at a time, NACKs on the command and on the read.
- **The budget:** at 400 kHz (DD-052) the command takes about 50 µs and the
  read about 0.15 ms, both in the handler, so a conversion is ready about
  9.3 ms after the top of the loop that started it, and the next top is never
  sooner than 10 ms (`test_ms5607_ready_before_the_next_loop` asks for 0.5 ms
  to spare; at 100 kHz it was ready at 9.87 ms). A loop that finds it still in
  flight counts a `pres_waits` and skips that loop's sample; it never reads
  early.
- **The cost:** the handler holds core0 for the command and the read, about
  0.2 ms in every 10. `log_rate_hz`'s default of 50 is at or above
  `SENSOR_RATE_HZ`, so every sample is logged: about 90 rows a second, nearly
  twice the flash written per flight, until the T8 logging-rate decision is
  made.

### DD-050: A Failed Sensor Never Deploys Anything
- **Decision:** the pressure layer marks a fit suspect while its window holds
  a gap longer than 250 ms, or a whole window of one reading, and for a whole
  window after either (SNS-PRES-10, SNS-PRES-11). A suspect fit is never
  clean. It holds the Mach lock and every pressure trigger with no time limit,
  and cannot set the Mach flag, arm the pyros or feed the emergency ladder. A
  whole window of one reading is a stuck sensor. No sample for 0.5 s in
  flight is a lost one. Each sets a DIAG bit, which `/api/status` lists by
  name, and logs SENSOR_STUCK or SENSOR_LOST, with a telemetry line. The
  flight carries on once the sensor answers again.
- **Why:** the Mach prompt requires that a failed sensor never cause a
  deployment, and so does DD-022. Out-of-range readings were already
  discarded (DD-036); nothing noticed the other failures. A stuck value reads
  as a rocket that has stopped.
- **250 ms** is longer than a flash stall (73 ms) and a sample together, so a
  stall is never a gap. A real sensor with the MS5607's noise never reads as
  stuck in an hour on the pad.
- **What the tests found on the way:**
  - A gap longer than the window left only new samples in it, too few to
    see the gap: the main fired 0.16 s after a 2 s loss. A gap ending at the
    window's edge now counts.
  - A stuck sensor coming back jumps by the climb it missed. The 40 ms rate
    read that as a supersonic climb and set the Mach flag, which then could
    neither release nor fall back: a flight that never deployed. Suspect fits
    no longer flag, and a flag from an unclean fit takes the sample's reading
    as p_flag, not the spoiled fit's pressure.
  - Arming needed the speed between 0 and 10 m/s, a window about a second
    wide just before apogee. Half a second of rejected readings there closed
    it for good. Arming now needs only "below 10 m/s", since arming late is
    safe and apogee has its own tests (DD-017).
- **Two corrections to T5 (DD-048), found through M2's closed-loop guards:**
  - σ was measured through the first 50 Pa of every launch, which pass the
    ground's gate. A fit through the ignition inflated it by 15-90 %, and a
    loose σ let spoiled fits count as clean. σ is now frozen at launch to its
    value from a second before T+0 (`test_T5_sigma_ignores_the_launch`).
  - With σ honest, a main set 60 m below a drogue that opens at 105 m/s
    waited out the opening's shock and fired 9 m low. After a charge, an
    unclean fit is now believed if it reads no lower than the last clean fit
    carried on ballistically (PYR-MODE-06). A bay charge reads the rocket
    lower than it is; a canopy opening reads it higher than a ballistic
    fall. The 5 kPa ejection test still holds the main to 0.6 m.
- **The honest σ moved the lockout's figures** (DD-049): the locks now let go
  at Mach 0.34-0.49, 8.5-16 s before apogee, and the mid-Mach flights 2.2-2.6 s
  after burnout. `test_M1_mid_mach_releases` allows 3 s: the burnout's step
  leaving the window, slowing below the release speed, then the release's
  second.

### DD-049: The Mach Lockout
- **Decision:** the Mach gate goes, and the prompt's lockout replaces it
  (`docs/mach_lockout.md`, FLT-MACH-02..07). A latch inside ASCENT:
  - **flag** when -ṗ > 0.029·p, from T+0;
  - **release** after a second of clean fits climbing slower than
    -ṗ < 0.022·p and decelerating at p̈ ≥ 0.0009·p, with p_min restarting
    there;
  - **fallback**, if it never releases: a second of clean fits showing the
    rocket falling back past the pressure it was flagged at. That declares
    apogee and arms what arming missed.

  While locked there is no apogee and no p_min. No channel arms before
  p < 0.9965·p0. A recovered ascent starts locked. The reported peak is the
  height at p_min, marked a lower bound if the lock let go within 2 s of
  apogee. The thresholds compare in integers (`src/mach_lockout.h`).
- **Why:** the gate latched above 100 ft/s, which nearly every flight passes,
  and believed the data after one second below it. A port's supersonic error
  can fake exactly that: on M0's low-drag flight a port that made the boost
  read as a descent fired the drogue 39 s before apogee, at Mach 1.27.
- **What M0's harness shows now:**
  - every profile is flagged by Mach 0.82, before its port error can begin;
  - every lock lets go at Mach 0.34-0.49 after burnout, 8.5-16 s before
    apogee, and at least 14.3 s before it on the low-drag flight to 10 km from
    the hot pad, over 1000 seeds (after DD-050's correction to σ);
  - every drogue comes 0.38-0.50 s after apogee, with the port error of
    either sign or none;
  - M0's fakes-descent port, scaled from 0.1 to 4 times either way, never
    moves the release into the error;
  - a port reading 15 % of q low read the draggy flight's peak as 1976 m for
    a 1526 m apogee; it now reports 1526 m.
- **Deviations beyond M1-D** (`docs/mach_lockout.md` lists them all):
  - **The flag is evaluated from T+0,** on the pad's samples. At 66 g the
    launch detector's 100 ft and 100 ms come after Mach 1, and the first
    flag was at Mach 1.04-1.09. A pad flag outlives a rise that falls back,
    because a port faking a descent reads the climb below the pad. It is
    forgotten after 10 s with no launch.
  - **Before the first release the flag also reads the rate over the newest
    two intervals.** Through a 66 g boost's first second the one-second fit
    still holds the pad and reads the climb 120 m/s slow.
  - **The fallback needs both conditions for the whole second,** not the
    rise alone: a later drogue, never an earlier one.
- **Found on the way:** the first pad flag cleared when the rise fell back.
  The port-error sweep caught the fakes-descent port "releasing" the lock at
  Mach 0.92, mid-boost, by pushing the sensed height below the pad.
- **Supersedes DD-025.**

### DD-048: One Estimator, A Quadratic Fit To The Last Second Of Pressure
- **Decision:** the pressure layer fits a least-squares quadratic through the
  median's output over the last second at every sample (`src/pressure_fit.c`,
  SNS-PRES-09), against each sample's own time, and evaluates it at the newest
  sample. The fitted pressure, rate and acceleration become a height, a speed
  and an acceleration through the altitude formula's slope at the fitted
  pressure, none of them clamped. From them:
  - every detector's speed (FLT-ASC-02);
  - the height AGL and FALLEN compare, and FALLEN's peak, the lowest pressure
    a clean fit showed (PYR-MODE-05);
  - `under_thrust`, from the acceleration (FLT-ASC-03).

  A fit is clean when its residual RMS is within 2σ and every residual within
  4σ. σ is the fit's own residual noise, measured on the pad over about five
  seconds, floored at the MS5607's 1.2 Pa and capped at 5 Pa, and frozen at
  launch to its value from a second before T+0 (DD-050). The pad marker
  (version 2) carries it to a recovered flight, and `/api/status` shows it as
  `fit_sigma_mpa`.
- **Why:** speed was a two-point difference of the filtered height, computed
  in three places. It trailed the rocket by the filter's time constant, so
  apogee came late, a SPEED channel fired 4.7 m/s past its setting in free
  fall, and AGL needed DD-029's lead. The Mach lockout (M1) needs the rate,
  its change, and a test of whether the samples can be believed. Only a fit
  gives all three.
- **Apogee (FLT-APO-01, T5-A):** clean fits show the pressure rising for
  60 ms, and the fitted pressure has risen to 1.0001 times the lowest a clean
  fit showed in ASCENT. Over 1000 flights from 100 m to 9 km, half of them
  through core0's stalls, the decision's sample is +0.004 s from the moment
  the true pressure first stands 1.0001 above its minimum (-0.17 to +0.08 s),
  0.41 s after the true apogee on average, and never before it. It was +0.21 s
  from the drop and 0.61 s after the apogee.
- **DELAY (PYR-MODE-05)** counts from where the fit's rate crossed zero, pdot
  over pddot before the decision: within 0.05 s of the true apogee plus the
  delay. It was 0.71 s late.
- **The pressure triggers wait out an unclean fit (PYR-MODE-06).** AGL, FALLEN
  and SPEED act on a clean fit. An unclean run is waited out for at most 2 s,
  then believed, and the wait restarts at each charge. A run ends only once
  fits have stayed clean for a whole window, so a lone clean fit under a
  swinging canopy does not start the wait again; two mains fired 25-32 m late
  before that. `test_T5_descent_glitch` found a defect older than the fit:
  two bad readings in a row under the drogue fired the main up to 236 m
  early, through the filter and DD-029's lead. Now within 0.4 m. A 5 kPa bay
  charge at the drogue fired the main at apogee; now within 0.6 m of its
  setting, and within 1.0 m under 6 Pa of canopy swing.
- **The launch reads the two-point speed while the fit is unclean
  (FLT-LAUNCH-07).** A burst of bad readings that passes the median spoils
  every fit holding it for a second, far longer than the 100 ms hold. The
  two-point speed spikes only for as long as the burst.
- **Deviations from the task as written:**
  - `under_thrust` ends 0.56-0.84 s after burnout, not within 100 ms. At a
    step in acceleration, the endpoint of a one-second fit crosses zero only
    once the thrust's share of the window has shrunk to 1/(1 + g) of it. A
    100 ms answer needs a window of 0.1-0.2 s, whose acceleration noise at
    9 km exceeds 1 g. `test_T5_under_thrust` holds it to 1 s, with one
    change of state.
  - `ARM_SPEED_CMS` stays at 10 m/s, now of true speed (DD-017). At 20 m/s the
    integration suite's A8-3, 19 m/s at 100 ft, never armed.
  - Floating point, not the Mach prompt's integers. The sums are float, the
    3x3 solve double, on the RP2040's ROM routines. The cost is a bench check
    still owed: at most 500 µs a sample.
  - Pressure triggers wait on any unclean fit, not only after a charge.
- **The Mach report on the fit:** every drogue fires 0.38-0.50 s after
  apogee. The low-drag "fakes descent" flight no longer fires at Mach 1.27:
  its fits are unclean through the port error, and its speed swings from
  -2271 to +1377 m/s without resting in the arming band. That is this port
  model's error changing faster than a quadratic, not a lockout. M1 is still
  needed.
- **Also:** pad speed noise 0.19 m/s RMS (was 0.22); speed error at 100 m/s,
  against the truth at the sample's own time, 0.8 m/s worst (was 6.1), 0.7
  through stalls (was 6.5).
- **Tests adapted.** `test_T11_stalls_change_nothing` set a sample's speed
  against the truth at the later tick that read it. The lag was invisible
  beside the filter's 6 m/s, but not beside the fit's 0.8 m/s. In
  `test_T11_loop_clock_independent` the igniters now burn through: the
  drogue retry it had been timing runs its grace on the loop clock by design
  (PYR-REFIRE-01). `test_REV03_refused_retry_is_asked_once` was "not settling"
  at 15 m/s only because the old filter took over 2 s to settle; it now falls
  at 40 m/s, faster than any canopy. The unit apogee test flies a parabola
  through the peak. The integration thrust test flies an A8-3 that burns out
  before the launch is declared.
- **Rejected:**
  - Precomputed coefficients: a stall leaves a gap and moves every later
    sample.
  - A Kalman filter or an alpha-beta tracker: each needs tuning to the
    vehicle.
  - A refit that drops outliers: at 100 Hz, a burst the median lets through
    can be six readings.

### DD-047: The Flight Log Carries Each Sample's Reading
- **Decision:** the flight log gains two columns before `event`: `raw_pa`,
  the reading the sample is centred on, after the range check and before the
  median, and `temp_c`, the sensor's temperature (DAT-02). Text rows gain two
  empty fields. `sim/replay.c` feeds a log's readings back through the
  pressure layer and detectors and sets the decisions against the log's own
  event rows, as `pyro_sim --replay <log>` (DAT-08).
- **Why:** the log carried only the filtered pressure, so a change to the
  filter or the detectors could not be checked against a real flight.
- **Why the centred reading:** each sample is the median of three readings,
  stamped with the middle one's time, so the middle readings, one per row,
  are the flight's whole reading sequence in order. Fed back, they reproduce
  the same medians and the same filter. The replay matches every event of the
  host's test flight and of three simulated flights (300, 1000 and 3000 m) to
  the millisecond.
- **Before `event`**, not after: a text row's last field is free text, and the
  web UI finds columns by name.
- **The HALs fill the columns themselves** from `pp_last_read_raw_pa()` and
  their own temperature, so `hal_log_sample()` keeps its signature.
- **The logging rate stays open** (T8): its default is already the sensor's
  rate, so a default log replays.
- **What building it found:** the replay started its filter with a stale time
  and needed one more reading to drain the median's last sample. It also
  showed the hold bug in DD-046's first version.

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
- **A hold's start is stored as its sample time plus one**, and read back as
  `ts + 1 - since`, so 0 can still mean "not started". The first version
  stored `ts | 1` and read `ts - since`. On an even sample time that read as
  4 billion milliseconds, so LANDED came on the first still sample with no
  hold at all, and so could a ground re-seed (DD-045). The host harness's
  samples happen to fall on odd milliseconds, which hid it; T8's replay, whose
  are even, found it. `test_T11_landing_holds_a_second` and
  `test_T6_rejecting_starts_at_zero` now run both parities.
- **Amended by DD-051:** the driver stamps each reading, not the HAL. The
  MS5607's stamp is the one-shot's handler's, taken as it commands the
  conversion; the BMP280's is `bmp280_read()`'s. The HAL passes them on.

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

