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
- **Superseded by DD-022:** the timer is removed; no timer may force apogee.
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
- **Amended by DD-057:** on USB, with test mode off, a reboot is obeyed.
- **Superseded by DD-058:** the API is live in flight; the filesystem is not.

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

### DD-096: lwIP Holds What The Rings Do Not
- **Decision:** lwIP's memory is sized to the HTTP engine's rings, not to
  its own defaults. A connection's bytes wait in its two 2 kB rings; lwIP
  holds two segments each way beyond them (`TCP_SND_BUF`, `TCP_WND`). The
  receive pool is 12 buffers, where it was 24; the heap is 20,000 bytes,
  where it was 8,000; the segment pool is 32. Asserts are compiled out and
  IP reassembly and fragmentation are off: one link, one MTU.
- **Why:** the heap was 8,000 bytes against a 5,840-byte send buffer for
  each connection, so two or three streams exhausted it (N1, DD-070), while
  36 kB sat in a receive pool whose high-water mark was 12 under two
  uploads at once.
- **Measured on MK1C,** one load round (the stream check, twelve fetches
  three at a time, six at once, two 200 kB uploads beside a fetch):

  | | Before | After |
  |---|---|---|
  | Heap refusals | 5,483 | 0 to 2 |
  | Heap high-water mark | 7,916 of 8,000 | 17,540 of 20,000 |
  | Receive pool high-water mark | 12 of 24 | 8 of 12 |
  | A 74 kB file, one stream | 0.14 s | 0.15 s |
  | Flash, MK1B image | 374,272 | 358,144 |
  | RAM, MK1C (data + bss) | 253,236 | 246,644 |

  The two refusals left are a write of about 2 kB finding no single free
  block; the transport offers it again on its next pass.
- **Two segments, not one or four:** a host that acknowledges every second
  segment answers a pair at once, and four each way bought no speed on a
  link whose round trip is a millisecond.
- **What it does not change:** 16 connections may still be open against 4
  exchanges. `docs/smallest_tcp_evaluation.md` has the case for replacing
  the stack; this makes the memory fit either way.

### DD-095: The Review Of 2026-10-02, Applied To Main
`docs/code_review_2026-10-02_resolution.md` lists each finding. What it
decided beyond the fixes themselves:
- **Two more start-up faults.** An image built for another board is FAULT at
  every boot (FLT-BOOT-17), not only after an update. A `config.ini` that is
  there and cannot be read is FAULT and the file is kept (FLT-BOOT-18): only
  a missing file is written with defaults. Both sound the general fault.
- **A configuration value its field cannot hold is refused** (SYS-CFG-03).
  The field keeps what it had; a post carrying one is answered 400 and
  nothing is stored. Board limits still clamp (PYR-BOARD-02): refusal is for
  what cannot be represented.
- **Same origin.** A POST needs `X-Pyro: 1` and a Host that is the board's;
  no response grants another origin access (WEB-API-07). A browser page
  from elsewhere cannot set the header without a preflight the board does
  not answer. A firmware update and BOOTSEL are still never refused for the
  flight's state (DD-094).
- **Flight events reach a script through the flight's own record.** The
  flight keeps its last eight events and the Lua service follows them
  (LUA-RUN-02); the flight software still calls nothing in Lua (SYS-LUA-02).
- **The script checker's arena is 16 kB**, static, where the reference used
  20 kB: MK1C's RAM does not hold 20. A script needing more to compile is
  refused at save. Nothing in the firmware calls `malloc`.
- **The bench's side of the application is built for size**: the HTTP
  server, the status and configuration text. With Lua's numbers truly
  32-bit, MK1B's image is 374 kB of its 384 kB slot.
- **The cost:** RAM on the Lua boards is nearly full. MK1C has about 700
  bytes between its data and the 2 kB heap reservation, which nothing uses.

### DD-094: Rulings Of 2026-10-03 On The Open Decisions
The designer's answers to `docs/outstanding_tasks.md` section 2.
- **Any fault found before launch sounds a fault (SNS-PRES-17).** A sensor
  that stops or sticks on the pad is a general fault, announced and named,
  until the board is restarted. Seen twice on the bench: a BMP280 left the
  bus and the board went on saying OK to fly.
- **Readings beside a flash operation (FL-1):** the data is left as it is
  pending a discussion of the root cause. Nothing changed.
- **The tests' 9 Pa is margin (NS-1).** Boards on a battery measure 1.9 to
  2.6 Pa; 9 Pa was MK1B on USB alone. The tests still fly at it.
- **MK1B's slot (SL-1):** a larger flash is on the way. While the image fits,
  nothing changes.
- **The three speed defaults stay 0 (R-1).**
- **Every pyro pulse is ended by hardware (R-2):** a hardware timer, or a
  fully defined PIO one-shot, for all pyro operations on every board. To
  build.
- **Every build runs a script (R-3):** the default script is a hello world
  that prints the flight's events as they change and claims no I/O, so
  MK1C-SD, whose J3 carries its SPI bus, runs one too. To build.
- **The flight log is prepared on the bench (C6).** A Log tab downloads,
  erases and prepares the log. Preparing creates an empty binary log for a
  chosen duration at the chosen rate, 1 Hz and 2 minutes by default: the file
  is opened, erased and closed on the bench. On the pad it is opened before
  OK to fly sounds, so the pad is quick. A start-up is the pad unless USB is
  found; then every file is closed and the board is USB's. In test mode the
  log records the test. A written log stays until the user clears it on the
  bench. To build.
- **A firmware update and BOOTSEL are obeyed in flight (L5).** Normally
  impossible, and wanted in a chamber test to stop a run and reload.
- **A board's address (ID-1):** every board first takes its address from its
  identity, which is the flash's id, not a serial number, and two boards can
  share it. When a board first registers, the web page sees that it has no
  stored MAC and gives it a random one, which gives it a random address. The
  MAC is kept in littlefs: it survives a firmware update, and after a whole
  flash erase the web page assigns one again. To build.
- **littlefs goes to v2.11.3 now (LFS-1)**, and to v3 once the fork's
  changes are ready.

### DD-093: A Free-Running Pressure Collector That Recovers Its Own Bus
- **Decision:** `src/pressure_collector.c` is one interrupt state machine for
  either sensor. It commands a conversion, waits out the part's worst-case
  conversion time on an alarm, reads, and commands the next, with no start
  from the loop. Each cycle is the part's raw bytes and the time of each
  conversion. A queue holds four; a fifth pushes the oldest out, counted.
  The sensor task takes them every loop and does the arithmetic. A sensor is
  a table (`MS5607_PART`, `BMP280_PART`): its commands, its conversion times,
  where its result is read, and its reset. Replaces the one-shot of DD-051
  and DD-066 and the BMP280's loop-driven cycle of DD-067.
- **Why:** the loop started every conversion, so the collector ran at the
  loop's rate and a loop that came early found it busy. A read can now only
  follow its own command's alarm, so a zero code (MS5607 datasheet page 11)
  is impossible by construction, not caught afterwards.
- **Rates:** MS5607 at 400 kHz, 18.8 ms a cycle, 53 a second. BMP280, 13.7 ms
  at 400 kHz and 14.5 ms at MK1B's 100 kHz. All faster than the 20 ms loop.
- **No wait in the handler:** a transfer is queued whole (the controller's
  TX FIFO holds sixteen) and its outcome is read at the next alarm. The old
  handler polled for the STOP.
- **Recovery is states of the same machine:** a failed transfer is counted by
  cause and the cycle starts again 1 ms on. A transfer that did not finish is
  aborted (RP2040 datasheet 4.3.10). Three failures in a row: the pads are
  taken from the controller, the controller is reset, nine clocks and a STOP
  go out at 10 us an edge (UM10204 3.1.16), the controller is set up again as
  it was, the part's reset command is sent, and its start-up is waited out
  (MS5607 page 12). About 4 ms. SNS-REC-01 is reworded: the flight software
  still does not recover a sensor; the collector recovers its bus.
- **No judgement of a value:** the zero check and the 1 Pa to 130 kPa check
  are gone (SNS-PRES-06). `support/compensation_range.py` shows neither
  sensor's arithmetic bends, wraps or overflows on any code. What stays is
  the logarithm's domain: a pressure of zero or less is not fed to the
  estimators.
- **Accuracy, in the task:** the MS5607's second-order compensation below
  20 C (datasheet page 9; 277 Pa at 0 C and sea level), and its PROM's CRC
  checked at detection (AN520).
- **What the build's proof caught:** the handler must run from RAM
  (`support/prove_core0.py`). A `switch` became a jump table in flash, a
  struct assignment became `memcpy`, and a `%` became a library call. The
  handler is an if-chain compiled without jump tables, builds each cycle in
  its queue slot, and wraps its index by comparison.
- **Unchanged:** a conversion a flash operation ran beside is still marked
  and not used (DD-068). Detection still uses the SDK's calls, before the
  collector has the bus.
- **Every sample in its loop (FLT-RATE-06):** the flight step took one sample
  a loop. At 53 samples to 50 loops the samples queued, and within 20 s every
  decision was made on data 1.2 s old. Found on the first bench flight on
  hardware: launch declared at 80 m instead of 32 m, the ground reference
  47 m high. `dispatch_state()` now steps once for every sample waiting. The
  host harness steps every millisecond and could not show it;
  `test_FLT_RATE_06_...` now does.
- **The bench profile carries the sensor's noise (SIM-02):** a landed profile
  was one value repeated, which is a stuck sensor, and the machine never
  landed. Each reading's own departure from the ground reference now rides
  on the profile.
- **On the bench, 2026-10-03:** MK1C, MK1C-SD and MK1B (MS5607) at 53.0 a
  second, 18.87 ms apart to within 0.1 ms; MK1A (BMP280) at 72.8 a second.
  No zero, no failed transfer, no cycle pushed out, all three PROMs pass
  their CRC. A 3 km bench flight on MK1C-SD: launch at 32 m, apogee called
  at the profile's apogee by both estimators, peak 3000.06 m against 3000,
  main at 297.8 m against 300, landed at 0.1 m. The recovery states have run
  on the host's fake bus only.
- **The cost of believing every value:** half a second of readings 1500 hPa
  high a second before apogee used to be discarded. They are now followed,
  and the drogue comes 8.6 s after apogee on that test, never before it.

### DD-092: Estimators Behind One Interface, All Flown, One Obeyed
- **Decision:** the filtered state comes from an estimator behind
  `src/estimator.h`: a table of functions and a name. The build carries the
  ones `src/estimator_table.c` lists. Every one is fed every reading. The
  flight obeys the one `estimator` in config.ini names, and logs what the
  others report and when each would have called apogee.
- **The two carried:**
  - `lumped`, the default: one equation of motion from pad to ground,
    dv/dt = a_T - g - beta (rho/rho_pad) v|v|, learning a_T and beta from the
    readings (`docs/lumped_parameter_filter.md`). `src/estimator_lumped.c` is
    the C port of `sim/study/lumped.py`, and
    `sim/study/lumped_port_check.py` flies both on the same readings.
  - `constacc`: the constant-acceleration filter of DD-085, unchanged, behind
    the interface.
- **Replacing one:** in a build, link another table. In the field, set
  `estimator` and restart.
- **The Mach flag is gone (FLT-MACH-02..07 withdrawn):** an estimator says
  whether its model explains the readings. Apogee is the obeyed estimator
  seen climbing, then seen falling, explained throughout; a fall whose climb
  was not seen must last 2 s (FLT-APO-07). A port error near Mach 1 is
  readings the model does not explain, so nothing is decided on it and none
  of it is the peak (FLT-APO-08).
- **One missing reading is not a gap:** the second of evidence an estimator
  needs starts again only after 250 ms without a used reading. A sensor that
  repeats itself for eight readings near apogee otherwise cost 2 s, on 2 of
  1000 flights to 10 km.
- **Measured, host flights obeying each (`test/test_mach.c`):** hop to 45 km,
  cold and hot pads, clean ports and three port errors. `lumped`: never
  early, 0.09 to 0.94 s after apogee. `constacc`: never early, 0.09 to 1.8 s,
  and later than 2 s on two of the 30 km and 45 km cases.
- **The log:** `EST` text rows, rationed as script output is. LOCK, UNLOCK
  and LOCK_FALLBACK are no longer written; PEAK and PEAK_AT_LEAST carry the
  peak, and the web page reads them, and still reads an older log's rows.
- **The cost:** every reading runs both filters, and each sample carries
  every estimator's state.
- **A prediction stops at the ground:** the model has no ground. With no
  readings (a sensor repeating itself), its prediction fell on through, to
  19 km below the pad after ten minutes. A prediction with no reading behind
  it now stops at the pad's level.
- **Flown on the bench 2026-10-03 (DD-093's entry).**
- **Open:** `lumped` anchors its air at the first reading's pressure and the
  sensor's temperature then; a long wait on a pad that warms is not yet
  followed. On the pad its speed reads about 0.5 m/s downward: the thrust
  term's fade pulls against the readings.

### DD-091: What Building The 2026-10 Review Settled
- **Apogee bounds (FLT-APO-01):** measured on the firmware itself, on host
  flights from 60 m to 30 km at sensor noise from 1.2 to 9 Pa: apogee is
  declared never early, and within 0.5 s to 10 km, 1.5 s at 20 km and 2.5 s at
  30 km. 9 Pa is the noisiest bench board's measured figure (MK1B); the tests
  fly at it. (DD-094: that figure was the USB supply; 9 Pa is kept as
  margin.)
- **Apogee under the Mach flag (FLT-MACH-04):** a plain "pressure rising for
  N seconds" rule fires early under a large port error, so the rule is the
  gravity signature: the state agreeing with the readings and descending
  slowly under gravity for 2 s. The older rule, the pressure back above the
  level the flag was set at, is kept as a last resort only. Beyond about
  Mach 2 with the test port model both can still be early: that is outside
  the envelope the tests cover.
- **OK to fly must be heard (BUZ-CODE-08):** silence means a fault, so a
  personality with a silent OK to fly is refused.
- **The announcement does not wait for the script (SYS-LUA-02):** the pad
  verdict used to be held until the script had started. It is not.
- **A commanded restart is not a failed start (LUA-SAFE-01):** the reboot
  route clears the script's start-up mark before it resets.
- **A script that fails its check is not saved (LUA-MGT-01):** the page checks
  first and uploads only a script that passes.
- **MK1C never abandons a pulse (PYR-ARM-03):** the gate closes at the
  precharge deadline on whatever the bus has; the timeout is that pulse's own
  record. U9's latch is released by the enable falling after every fire
  (TPS2595 datasheet page 23), which is what PYR-FAULT-01 asks.
- **The bench profile's descent starts from rest (SIM-04):** it gathers speed
  under gravity toward each rate, so a failed canopy is a fall and no speed
  appears from nowhere.
- **No hold on launch or apogee (task T3, 2026-10-03):** the 100 ms launch
  hold and the 60 ms apogee hold are removed. With either at zero every pad,
  flight and Mach test passed, 30 minutes of 30 Pa gusts at 9 Pa of sensor
  noise included: the filter's own lag and its skipping of one or two bad
  readings are the guard. `launch_detected()` is a pure function.
- **MK1C's presence pulse is ended by a hardware alarm (task P1):** 8 ms, not
  the next 20 ms loop. A fire drops a test in progress, and no storage write
  starts beside the pulse.
- **Open:** the three speed defaults are 0 until the modelling study gives
  values; MK1A and MK1B do not meet PYR-ARM-01's 50 ms; MK1C-SD has no script
  (SYS-LUA-01); HAL validation is not built (`docs/hal_validation_apps.md`).

### DD-090: Ten Code Requirements, And The HAL As The Test Seam
- **Decision:** the code itself has requirements (CODE-01..10): structure
  and naming before comments; comments for traceability and for decisions
  the structure does not show, never to teach a subject; named functions
  before magic values; one responsibility per file; SOLID and DRY; black-box
  tests traceable to requirements; all code reachable through the published
  interfaces, with no dead code, no test-only functions and no test
  constructs that reach hidden functions; pure functions preferred.
  Libraries from outside the project are exempt and are not modified.
- **The seam:** the HAL interface is the mockable layer through which all
  flight code is tested (HAL-05), and it makes a port a new HAL. The HAL is
  validated separately, on hardware, with test equipment and HAL validation
  applications (HAL-06, BLD-06).
- **Why:** given by the user on 2026-10-03, after the requirements review,
  to govern the review of the tests and the code that follows it.

### DD-089: A Script Cannot Block The Board
- **Decision:** Lua is a functional requirement on every board (section 10
  of `REQUIREMENTS.md`). No script may block, prevent or delay any behaviour
  of the board: starting up, the network, the announcement, logging,
  telemetry, any flight function (SYS-LUA-02). Whatever script is stored the
  board starts and serves its web interface (LUA-SAFE-01). A script has full
  control of the pads the pin assignment gives it, a pyro channel's
  included, and that use is the operator's. The enabled script always runs,
  ground test and bench flight included (LUA-RUN-01).
- **Why:** the user's rulings in the 2026-10 review. A script that could
  hold up a start would turn a bad upload into a board that cannot be
  repaired from its own web interface.

### DD-088: Telemetry Is $PYRO Once A Second, And Carries Its Events
- **Decision:** the downlink is the $PYRO sentence, once a second in every
  state it is sent in (TEL-03). A flight event is queued and carried by the
  next message, so it can be up to a second late and none is lost (TEL-11).
  The configurable format and rate are withdrawn, with `telem_format` and
  `telem_rate_hz`. The port accepts no commands (TEL-12). A user who needs
  another format assigns the serial pins to a script.
- **Why:** the user's rulings in the 2026-10 review. The log may run faster
  than the downlink; the downlink is for watching a flight, not for
  reconstructing it.

### DD-087: Ground Test By The Switch Alone, And Configuration At Start-Up
- **Decision:** the serial ground test commands are removed (GND-TEST-01..04
  withdrawn). The test switch procedure of DD-071 is the only ground test,
  and its fires are delivered on command with no health gate (GND-TEST-13).
  The switch may connect the buzzer's pad to another pad and may not ground
  the buzzer; pad to pad, the input pad watches the buzzer's own pattern and
  short pulses find the switch while the buzzer is silent (GND-TEST-12,
  `docs/ground_test_on_buzzer_pad.md`).
- **Configuration:** configuration and pin changes take effect at start-up
  (CFG-10). The web interface writes the file; the running system, ground
  test mode included, goes on with what it started with.
- **Why:** the user's rulings in the 2026-10 review. The serial commands
  needed a computer, which the user need excludes; and a save that only
  writes a file cannot be refused for the mode the board is in.

### DD-086: Any Restart Resumes A Flight In Progress
- **Decision:** recovery is no longer tied to a power event (DD-041's
  premise). After any restart the system resumes if the pad's record exists,
  the board is above the recorded ground and moving, and no USB host is
  attached (FLT-BROWN-02). A resumed flight assumes no channel has fired and
  fires as soon as fresh data meets a trigger; a DELAY counts in full from
  the resume; the emergency rule applies from the resume (FLT-BROWN-06). All
  resume state is cleared when the flight lands and when a bench flight
  ends (FLT-BROWN-04).
- **Why:** best effort. A watchdog reset in the air used to start cold, take
  a ground reference at altitude and fire nothing. A fire into a spent
  igniter is harmless, so nothing about the channels need survive.
- **Open:** restoring T+0, apogee and the peak by replaying the flight log,
  if it is fast enough (`docs/resume_from_log.md`).

### DD-085: One Estimator On Raw Pressure, And Every Operation In Pressure
- **Decision:** the detectors read one filtered state -- pressure, its rate
  and its acceleration -- formed from the raw readings by a Kalman filter
  whose correction is limited inside its update (SNS-EST-01, SNS-EST-02).
  No median and no low-pass stand in front of it. It discards nothing, is
  not re-seeded and is not loosened at a fire; it trends to data that is
  stable. Every flight comparison is in pressure, and a height or speed the
  operator set is converted once (SNS-EST-05). No altitude is clamped, at
  8000 m or at zero (SNS-ALT-01). This replaces DD-040, DD-044 and DD-048.
- **Apogee at altitude:** filtered pressure must show the slowing climb,
  apogee and descent at every height in the sensor's range. A fallback that
  waits for the pressure to return to the Mach flag's level is rejected: it
  deployed a good 30 km flight 150 m above the ground (HA-1). FLT-MACH-04 is
  rewritten and DD-049's fallback with it.
- **Data is believed:** the board cannot know its sensor is wrong. A reading
  beyond the rated range is used (SNS-PRES-06). A stuck sensor is a chip
  failure: it is logged and nothing recovers it in flight (SNS-REC-01).
- **Why:** the user's rulings in the 2026-10 review, and the evaluation in
  `docs/kalman_launch_evaluation.md`: a raw filter lets one reading 3 to
  6 kPa low declare a launch, and limiting the correction stops one or two
  bad readings of any size at no cost in delay. The design and what remains
  to be measured are in `docs/descent_speed_estimator.md` and
  `docs/pressure_domain_flight_math.md`.

### DD-084: A Board Declares Its Constraints
- **Decision:** the requirements are general, and a board may redefine a
  default or narrow a range: `refire_interval` and `fire_gap` for its
  protection part, its sensor's range and noise, its height for proper
  operation, its log capacity and log details (PYR-BOARD-01..04, DAT-09,
  SNS-MAX-01, BRD-01). An out-of-range value is brought to the nearest
  permitted one and reported, never rejected. A constraint may change when
  a channel is energised; it may never withhold a decided fire.
- **Why:** MK1A's low-current PTC needs a different gap from MK1C's eFuse,
  and a configuration file can arrive from another board.

### DD-083: Four Announcements By Priority, And Health That Gates Nothing
- **Decision:** the pad says one of OK to fly, pyro 1 fault, pyro 2 fault,
  general fault; silence is also a fault. One is announced at a time:
  general, then pyro 1, then pyro 2 (BUZ-CODE-02). Each board checks every
  enabled channel for the faults it can reliably detect; one that can detect
  none treats the channel as ready (PYR-HEALTH-01). A short cannot be told
  reliably from a match, so the general verdict is ready or fault and the
  measurement is named on the status report. Only enabled channels count,
  and the pin assignment is the source of truth (PYR-HEALTH-02).
- **Why:** the user's rulings in the 2026-10 review. A general fault goes
  back to the workbench; a pyro fault may be fixed at the pad, one at a
  time. Diagnosis is not for the beep.

### DD-082: Re-Fire, Emergency All-Fire, And The Gap, Without Roles
- **Decision:** after a channel's first fire it fires again every
  `refire_interval` while the descent speed exceeds that channel's re-fire
  speed (PYR-REFIRE-01). At any time after apogee, at
  `emergency_fire_speed`, every enabled channel fires and goes on firing
  until the speed drops or the flight lands (FLT-EMRG-01). A zero speed
  disables its rule. The channels are never energised together, and
  `fire_gap` of quiet separates a pulse on one from a pulse on the other; a
  first fire goes before a re-fire and pyro 1 before pyro 2
  (PYR-DEPLOY-02). "Drogue" and "main" leave the requirements. The rules
  act on the filtered state (FLT-EMRG-05). This replaces DD-028's ladder.
- **Why:** the user's rulings in the 2026-10 review. Any deployment reduces
  the damage, and a pulse into a spent igniter is harmless.
- **Open:** the defaults of the three speeds, zero until the modelling study
  gives them.

### DD-081: Never Early, Data Believed, Best Effort
- **Decision:** a fire is decided only on measured evidence that its event
  has happened: never early, never on an estimate, never on a timer
  (SYS-DEPLOY-04). Samples that arrive are believed; only their absence
  suspends a decision. Once decided, nothing withholds the attempt: health,
  continuity, faults and voltage gate nothing, in flight or in ground test
  (SYS-DEPLOY-05, PYR-HEALTH-01). PYR-SAFE-01 and PYR-ARM-02 are withdrawn
  and no fire is refused (PYR-FIRE-01).
- **Why:** the user's rulings in the 2026-10 review. No correction is
  possible after launch. A safe flight carries two pyro systems, so this
  unit prefers no deployment to an early or uninformed one, and never holds
  a decided one back.

### DD-080: Requirements State Behaviour; Mechanism Is In The Design Record
- **Decision:** `REQUIREMENTS.md` states behaviour that can be verified from
  outside and met by any implementation. It dictates no processor, kernel,
  bus, interrupt or algorithm; the hardware layer may use any of them.
  Requirements that were mechanisms are withdrawn (Appendix A) and stay in
  force as design, in the decisions they cite and in each board's theory of
  operation. The requirements are general, with board values declared
  beneath them (DD-084). FreeRTOS is the only task model supported, as a
  design fact (DD-073).
- **Why:** the user's ruling in the 2026-10 review, recorded in
  `docs/requirements_review_2026-10-02.md` with every identifier's
  disposition.

### DD-079: Descent Rates Are Judged In The Pad's Air
- **Decision:** the descent bands (a main at 10 m/s or less, a drogue at
  35 m/s or less) and the emergency ladder's evidence read the descent rate
  scaled to what the same canopy would give in the pad's air:
  `pp_air_scale()` multiplies the formula's speed by the atmosphere's slope
  over the formula's, and by sqrt(rho / rho_pad), with the 1976 US Standard
  Atmosphere's temperature at each pressure. It is 1 at the pad, 0.95 1 km
  above a sea-level pad, and 0.22 at 30 km.
- **Why:** found by the first 30 km profile the bench flew (DD-078). A
  parachute's terminal rate goes as 1/sqrt(rho): a drogue that settles at
  25 m/s over the pad falls at about 200 m/s at 30 km. Read raw, that was a
  failed drogue by FLT-EMRG-01's test, and the ladder put the main out at
  29 km, four seconds after apogee. The same flight with a drogue giving
  90 m/s of pad air still forces the main near apogee: the ladder still acts
  up there, on a rate no drogue explains at that height.
- **Why the formula's slope too:** the altitude formula,
  h = 44330 (1 - (p/p0)^(1/5.2561)), is the troposphere's. Above 11 km the
  air stops cooling and the formula reads short: 25.3 km at a true 30 km, and
  a speed 0.54 of the true one. The scale takes the true one first. The
  formula itself, and the 8000 m clamp on altitude (SNS-ALT-02), are
  unchanged.
- **Pinned by** `test_FLT_AIR_01_air_scale_is_the_pad_air_rate` against the
  bench flight's own atmosphere, and by closed-loop flights of 30 km from a
  sea-level pad and 20 km from a 1500 m pad.
- **Not changed:** a pad above sea level still reads its AGL triggers about
  3 % low (the formula assumes 288.15 K at the pad; at 1500 m the standard
  gives 278.4 K): a main set for 300 m fires at 290 m.

### DD-078: A Flight On The Bench
- **Decision:** on a board built with `PYRO_HAS_BENCH_FLIGHT` (MK1C and
  MK1C-SD), `POST /api/sim/flight` flies a profile (`src/flight_sim.h`):
  on the pad, a constant-acceleration boost sized for the apogee asked for,
  a ballistic coast, a drogue whose rate scales as sqrt(rho0 / rho), a main
  from its altitude, landing. Its pressure, from the 1976 US Standard
  Atmosphere to 32 km, replaces each reading after the pressure trace has
  recorded the sensor's, so the flight software, the flight log and the
  high-rate log fly it on the real board while the IMU logs the board as it
  sits. `GET /api/sim` follows it; `support/bench_flight.py` drives one.
- **Guarded:** it starts only from PAD_IDLE with test mode on (USB-08), and
  runs on the flight task through `flight_call()`. From its start until the
  board reboots, every fire is mocked and logged as one; a mocked fire reads
  energised for 500 ms, as MK1B's pulse does, so the machine records it as
  fired, and the channel reads open after it, as a lit charge does. The
  channels read good until fired, so the whole plan flies with nothing
  connected. A flight stopped part-way hands the machine the pad's pressure
  mid-descent, and a main set by altitude would fire into it, which is why a
  stop does not give the channels back.
- **Why not a replay:** the logger has to be judged over a flight the board
  has not flown, to the altitudes the user asked about ("the full high-rate
  logger works over high altitude flights"), and the chamber reaches none of
  them. A profile is short to describe and repeats exactly.
- **What it found:** DD-079, and that the Mach lockout does not release
  above its 9 km envelope, so a flight there gets its drogue from the
  fallback, 100-190 m above the pad (`docs/high_altitude_flight.md`, task
  HA-1, open).

### DD-077: The High-Rate Log On The SD Card
- **Decision:** on MK1C-SD, an LSM6DS3 on the SD card's bus (±16 g,
  ±2000 dps, both at 1.66 kHz into its FIFO in continuous mode) and a
  high-rate log on the card (`src/sd/hr_log.h`): every IMU set, every
  pressure and temperature conversion as the pressure trace records it, and a
  snapshot of the flight ten times a second. Two tasks at P on core1: a
  reader every 10 ms drains the FIFO and the conversions into a 32 kB
  lock-free ring, and a writer empties it onto the card in 4 kB stages. The
  flight task is not on its path.
- **When:** while the flight log runs, launch to landing, and on the bench
  from `POST /api/hr/start`. Between flights the ring keeps its newest half,
  so a log opens with the second before launch.
- **Power cuts:** the next file is created and preallocated contiguously on
  the pad (`f_expand`), so launch waits on no FAT search; it is
  `logs/next.bin` until it closes and is renamed `logs/hrNNNN.bin`, and one
  a power cut left is renamed at the next boot. Each record carries a
  CRC-16 of its payload, and the log ends at the first that does not match.
- **Why a card:** 1.66 kHz of six 16-bit values is 20 kB/s before framing;
  a 30 km flight is ten minutes, 12 MB. littlefs on the internal flash has
  neither the room nor the write rate, and its erases stop the other core
  (DD-074).
- **No interrupt pin:** none reaches the MCU, so the reader polls. The FIFO
  holds 682 sets, 410 ms at 1.66 kHz, so a reader late by less than that
  loses nothing, and the FIFO says when it did (FIFO_OVER_RUN). Reads take
  whole sets only, aligned by FIFO_PATTERN (AN4650, page 88).
- **A file the card cannot take any more** -- the card mounted again under
  it, or three writes in a row failing, whose error FatFs keeps for good --
  is given up: the log goes on in a new file from the next whole record, and
  the old one is renamed at the next prepare, which never creates over a log
  it could not rename. The expanded size is synced at prepare, so a log is
  recoverable whole from its first byte. The record a switch tears is the
  most lost (HR-06). Contiguous space is taken as the longest run the free
  space holds, halving from 128 MB.
- **On the bench, 2026-09-29:** 70 s at 1.66 kHz with a remount at 40 s:
  24 kB/s to the card, no record dropped, no FIFO overrun, the slowest 4 kB
  write 9 ms, the ring at most 17 kB of 32; the new file's first set 0.9 ms
  after the old one's last. A 30 km bench flight, launch to landing: 16.2 MB,
  1,088,033 sets over 668 s with none lost, 33,421 conversions with none
  missed, 6,684 snapshots.
- **Timing:** a batch is stamped at its FIFO status read, after the bus is
  had; stamped before, a stage holding the bus made batches look early.
  `support/hr_log.py` spaces sets at the sensor's measured rate (1627.8 Hz
  on this LSM6DS3, not 1660) and fits each unbroken run of batches to a line.
- **Decoded by** `support/hr_log.py`; host-tested by `test_hr_log.c`.

### DD-076: Every File Through vfs.h, The SD Card First
- **Decision:** every file the firmware opens goes through `src/vfs.h`,
  which routes a path to FatFs on the SD card while one is mounted, and to
  littlefs otherwise. The board's identity (`/serial.txt`) and the pad
  marker stay in littlefs on every board. FatFs is R0.16 with its two
  patches (`lib/fatfs`), with exFAT, long names, `f_expand` and its
  reentrant lock, over this firmware's SD driver (DD-075).
- **Configuration:** `config.ini`, `pins.ini`, `beep.ini` and
  `lua_user.lua` are the card's, and each is copied into littlefs whenever
  the two differ (`vfs_mirror()`, at mount and after every write), so a board
  whose card is missing or unreadable boots with the configuration it last
  had. A blank card is seeded from littlefs at mount. A file the card lacks
  is read from littlefs, so a blank card still serves the web pages.
- **Why:** at the user's direction: "Use the SD for all files, but copy the
  SD card configuration files to the littlefs storage when they are
  different."
- **Every call may block** on the store's lock; the flight task reads the one
  file it needs through `hal_fs_read_cached()` (DD-074) and writes through
  rings the storage task empties.

### DD-075: An SD Card And An IMU On MK1C's J3
- **Decision:** a board variant, `boards/mk1c_sd`, gives J3 (GPIO18-21) to
  SPI0 -- SCK 18, MOSI 19, MISO 20, the card's select 21 -- and J1.6
  (GPIO22) to the LSM6DS3's select. Lua is off on it: J3 was Lua's. The SD
  card runs in SPI mode with CRC on (CMD59) at 12.5 MHz, the LSM6DS3 at
  10 MHz, and each takes the bus for one transaction at a time, so a card's
  busy wait gives the bus back between polls (`src/sd/spi_bus.h`,
  `src/sd/sd_card.h`).
- **Waits:** every one bounded by the SD specification's limits -- reads
  100 ms, write busy 250 ms, 500 ms on SDXC (Physical Layer Simplified
  Specification 6.00, PDF page 97) -- and the card's initialisation by 1 s.
- **A command waits out the last write:** a card programs its last block
  after it is deselected, so the ready wait before every command allows the
  write-busy limit, 500 ms, not the read limit. With the read limit a
  4 kB-chunk bench counted 7 timeouts and 3 retries; with it, none in 16 MB.
- **Found on the bench, 2026-09-29: MK1C's 3.3 V cannot carry a card.** On
  J3.1 the card reset 28-29 ms into every initialisation -- a power-on reset,
  since only one returns a card in SPI mode to SD mode (SD simplified 6.00,
  section 7.2.1, PDF page 228) -- and moved the MS5607 on the same rail by
  10 Pa. The same clocking with the card deselected moved nothing, and a
  battery on VIN changed nothing: the card's draw into U6's foldback limit
  (XC6206, PDF pages 1 and 5). On its own MIC2920A-3.3 from VIN the card
  initialises at once (SDHC, 15.6 GB) and writes 734 kB/s in 4 kB chunks at
  12.5 MHz, 1020 kB/s at 20.8 MHz, with no CRC error; its programming pauses
  reach 155 ms. A board carrying a card needs a larger U6 or a regulator for
  the card (task C-U6).

### DD-074: A Flash Operation Parks The Other Core From A Task Raised To T
- **Decision:** any task but the flight task may write flash, one operation
  at a time, through `flash_op()` (`src/flash_op.h`): the caller raises
  itself to T, wakes the other core's lockout helper -- a task at T that
  exists from boot, pinned to that core -- which disables that core's
  interrupts and spins in RAM (`flash_op_park`), runs the operation with its
  own interrupts off, releases the helper and drops back to its priority.
  Every wait is bounded: a helper that does not park within 20 ms refuses
  the operation; a parked helper frees itself after 1.5 s. The flight task
  and interrupt handlers are refused. littlefs is built `LFS_THREADSAFE` on
  one mount made at boot, its program and erase callbacks go through
  `flash_op()`, and its caches are 1 kB, so one program operation stops the
  system for four pages at most.
- **Why not only the logger writes flash (plan 2, section 3):** under mode 0
  a task at T stops every task at P on both cores for as long as it runs. A
  single writer at T would run every file request -- an upload, an OTA
  image, a config save -- at T and stop the flight task for all of it; an SD
  card's busy time, hundreds of milliseconds, likewise. Raising a caller to
  T for one operation keeps the stop to the operation, which is what plan 2's
  4.2 rule 1 requires: the task that holds the lockout is at T while it does.
- **Why not the SDK's `flash_safe_execute()`:** it creates a task on the
  other core for every operation, on one static stack and control block when
  dynamic allocation is off, and raises its priority after creating it.
  Plan 2's 5.1 refinement, a persistent helper, removes both.
- **The pad marker:** the flight task reads it during a boot's recovery
  decision. `hal_fs_read_cached()` serves it from RAM behind a seqlock,
  filled at boot and refreshed by every write, so the read never waits.

### DD-073: FreeRTOS SMP, Plan 2, Mode 0
- **Decision:** at the user's direction ("Do a full refactor to the
  freertos plan"), the firmware runs on FreeRTOS-Kernel V11.3.1 SMP across
  both cores, as `docs/log_storage_plan2_freertos.md` lays out, with
  `configRUN_MULTIPLE_PRIORITIES` 0 as the user directed on 2026-09-28:
  - two priorities, P and T = `configMAX_PRIORITIES - 1`, checked at build
    time (`rtos_tasks.c`);
  - the flight task alone at P on core0, woken every 20 ms by an alarm on
    the hardware timer, never by the tick; its only blocking call is that
    wait, and `prove_core0.py` refuses every other blocking kernel call on
    its path by name;
  - the net task (TinyUSB, lwIP, HTTP, with the USB interrupt), the Lua task,
    the storage task and the timer daemon at P on core1, sharing it by time
    slicing; the tick interrupts core1 only;
  - a lockout helper at T on each core (DD-074).
- **What went:** the flash window, core1's time-boxed units and RAM idle
  loop, the grants, the FIFO launch and the PSM kill. Lua's stop is
  `vTaskSuspend()`. HTTP's work units all run in the net task.
- **What the flight task hands over:** the flight log's records go into a
  single-producer ring the storage task writes out; the pad marker and a
  drawn MAC are written by the storage task. Changes to state the flight task
  owns -- the running config, test mode, a buzzer audition -- reach it
  through `flight_call()`, a mailbox it empties at the head of its period.
- **The watchdog:** fed by the flight task, and on the ground only while the
  storage task checks in, so a stuck storage task resets the board there. In
  flight a stall is counted and the flight goes on.
- **A new image commits** to `pico_fota_bootloader` from the storage task
  after five seconds of every task running, not at boot, so an image that
  dies once its tasks start rolls back by itself.
- **Plan 2's open decisions, taken:** conversions a write disturbs are
  discarded (DD-068 already does it) rather than writes being placed in the
  sensor's idle stretch; a runaway script is preempted and its tick bounded
  by Lua's instruction hook; the network shares core1 with Lua.
- **Cost:** the kernel adds about 15 kB of code on each board; lwIP, TinyUSB
  and the kernel are compiled `-Os` so MK1B's 384 kB slot still holds the
  image. RAM rose about 15 kB on MK1C for the task stacks and the kernel.

### DD-072: The MAC Is Drawn From The RNG And Kept
- **Decision:** at the user's direction ("build the mac with the RNG"), a
  board with no `/serial.txt` draws its MAC at boot and keeps it there. The
  pool takes 2048 samples, each an ADC conversion of the temperature sensor
  with a ring-oscillator random bit and the timer's low bits beside it, and
  mixes them through splitmix64's finaliser (`mac_random.c`). 0x02 leads;
  the last byte, the subnet, is never 0, 1 or 255. The file is written once
  the filesystem is up, as twelve hex digits and a second line `rng`, and
  `/api/status` reports `mac_source`: `rng` or `assigned`.
- **Why:** the MAC used to be derived from the flash chip's unique id. Two
  MK1Cs read the same id from their XT25F128F, 41503459373331FF, and took
  one MAC, serial and subnet (task ID-1).
- **Why these sources:** the RP2040 has no hardware RNG. The ring
  oscillator's random bit is usable while the system runs from the crystal,
  "not ... for security systems" but "useful in less critical applications"
  (`rp2040-datasheet_2025-02-20.pdf`, section 2.17.5, page 223), and no
  figure is given for its entropy. The ADC's low bits are a second,
  independent source. A MAC needs 40 bits; the pool sees thousands.
- **What it does not fix:** the subnet is 8 bits, so two boards share one at
  the birthday rate whatever the randomness: about 4 % at 5 boards, 16 % at
  10. `/api/serial` still overrides.
- **Consequence:** a board without `/serial.txt` moves to a new subnet on its
  first boot of this firmware. `support/register_board.py` finds it again.

### DD-071: A Ground Test Procedure, By Switch
- **Decision:** at the user's direction, a ground test switch assigned to a
  pin. Powered up with it closed, the board enters ground test mode and
  announces it: three long beeps and a pause, repeating. Opening the switch
  starts the procedure:
  - a countdown at a count a second, five fast beeps down to none, and at
    zero pyro 1 fires;
  - a 3 s steady tone and a second countdown, and at its zero pyro 2 fires;
  - three long beeps, once, then silence.

  A channel not enabled is skipped: with one enabled, the tone and the second
  countdown go; with none, the countdown leads to the all-clear. The user's
  words: "When the pin is asserted on powerup, the unit enters ground test
  mode"; "step 5 becomes, 3 second tone, followed by the 5 count down. The
  count-down pace is 1 second per count."
- **The switch, two ways (the user's second change):** "a switch to ground"
  or "a switch that shorts two other pins", because "the mk1a ground pad got
  crowded". pins.ini says `ground_test=none|ground|pair`,
  `ground_test_pin=` and, across two pads, `ground_test_drive_pin=`.
  - Across two pads the driven pad alternates high and low each loop, and the
    read pad is pulled the opposite way each time. The switch reads closed
    only when the read pad follows both ways (`ground_test_switch.c`), so a
    read pad touching ground or the supply is never a closed switch.
  - The switch's pads are the flight software's: reserved against Lua,
    claimed at boot, refused if a script, the buzzer or the board holds them.
- **Choices made where the description was silent, for the user to confirm:**
  - *Enabled* means a configured mode other than none, and pads not released
    to Lua. The board's own refusals still apply (MK1C fires only a channel
    its tracking test has seen present); a refusal is reported and the
    procedure goes on.
  - *At power-up* means held closed for the last 0.5 s of the 2.5 s settle.
    The mode is taken after the sensor and continuity checks
    (PYR-SAFE-01), and a board recovering a flight after a power event keeps
    flying whatever the switch says.
  - A release counts only after the switch has been held closed 1 s in the
    mode, so the operator has heard the mode first; a switch opened during
    boot must be closed and opened again. A change counts once it has held
    100 ms.
  - Closing the switch again during a countdown or the tone stops the
    procedure before the next fire and announces the mode again.
  - The fire comes at the zero count, five seconds into the countdown. Five
    fast beeps (60 ms on, 60 off) take 0.54 s, clear of the next count.
  - The mode is terminal until the next power-up: no launch detection, no
    flight log, no $PYRO sentence, no pad announcement, and the USB attach
    chirp does not take the buzzer. `!GT` lines report each step.
  - The schedule is the sequence's, not the buzzer's: a board with no buzzer
    (MK1A as shipped) fires on the same clock, silently.
- **Contracts:** `hal.h` gains `hal_ground_test_asserted()`, with a weak
  default in `flight_states.c` so every build links, and
  `hal_ground_test_configure()` for `main()`. GROUND_TEST is appended as
  state 12, after FAULT, so no recorded state number moves.
- **Owed on the bench:** a person with the switch wired, both wirings, dummy
  loads on both channels, and a scope or meter on the firing pads.

### DD-070: The USB Network Holds A Frame The Endpoint Cannot Take Yet
- **Decision:** link output no longer drops a frame the USB endpoint is too
  busy to take. It holds it, by reference, in a queue of eight
  (`net_txq.c`), and `net_service()` sends the queue in order as the endpoint
  frees, before and after the HTTP transport. A frame is refused only when
  the queue is full or the host has let the device go. lwIP does not
  retransmit a segment a driver still holds. `/api/net` reports lwIP's pools,
  TCP's connections by state and what the transport refused (WEB-API-13).
- **Why (G4-N):** the board's HTTP outages of 40-60 s needed evidence, and
  `/api/net` gave it on the first G4 round: 472 of 1182 frames (40 %)
  refused at the endpoint, lwIP's heap out 27,865 times at a high-water mark
  of 7,832 of 8,000 bytes, and 158,937 refused TCP writes. The old link
  output gave up after 20 quick retries, far less than one full frame's
  1.2 ms on the bus, and left the frame to lwIP's retransmit timer: 3 s,
  doubling. Meanwhile the segment held the heap, and a SYN-ACK needs the
  heap too. That makes a stalled response, then refused connects, while
  ping, which needs no heap, still answers: G4-N's order of events.
- **After, on the bench MK1B (2.1.700, one G4 round):** 21 of 878 frames
  refused (2.4 %), 522 held and sent; heap refusals 4,493, TCP write
  refusals 4,493.
- **Not done:** lwIP's heap is 8,000 bytes against a 5,840-byte send buffer
  per connection, so two or three connections streaming at once still
  exhaust it. Raising it spends RAM the flight-code move would need; that is
  the user's call.
- **Hardware (2.1.701, G4 on all four boards):** 14-43 frames refused of
  about 1,400-4,600 each, 540-1,180 held and sent; lwIP's heap refused
  3,230-6,517 times, its high-water mark 7,656-7,976 bytes. Before the fix, a
  one-hour soak of three boards (2.1.697, about 32,000 trace requests each)
  met no outage: G4-N needs G4's parallel load, not the trace alone. On the
  fix, a 30-minute soak of all four boards (about 16,000 requests each,
  five G4 rounds on the bench MK1B meanwhile) met none; each board held and
  then sent about 17,000 frames, and no frame was refused and no heap
  allocation failed beyond what G4 had left.

### DD-069: No Sensor Bus Transfer Waits Without Bound
- **Decision:** every I2C transfer the loop makes to a pressure sensor gives
  up: the MS5607's detection and MK1B's BMP280 reset within 2 ms, the
  BMP280's reads and commands within twice their own time on the bus and a
  millisecond (3 ms for a loop's read at MK1B's 100 kHz, 5.5 ms for the
  calibration). A fixed 2 ms failed MK1B's BMP280, whose 24-byte calibration
  takes 2.3 ms at 100 kHz; the fake bus now times a transfer at its rate, and
  `test_bringup_mk1b_bmp280` caught it. `support/wait_check.py` now refuses the SDK's
  `i2c_write_blocking` and `i2c_read_blocking`.
- **Why:** at the user's direction, "There must never be a lockup in
  flight." The SDK's blocking transfers pass no timeout
  (`hardware_i2c/i2c.c:246` in SDK 2.2.0), so a part holding SCL low holds
  core0 until the watchdog resets it. A watchdog reset comes back cold, and a
  cold board in the air never deploys (`test_BRN_01_software_reset_never_recovers`).
  The BMP280 is read every loop in flight, and bring-up, MS5607 detection
  included, runs again after any reset in flight. The MS5607 one-shot's own
  transfers were bounded already (`MS5607_BUS_TIMEOUT_US`, DD-051). Found
  by an audit of the flight path for this task.
- **Tests:** a fake part that holds the bus, against a fake SDK whose
  blocking transfers count as an unbounded wait
  (`test_bmp280_a_held_bus_costs_a_bounded_wait`,
  `test_bringup_a_held_bus_is_bounded`,
  `test_bringup_mk1b_held_bmp280_is_bounded`). The two bring-up tests failed
  on MK1B before the change.
- **The proof, too:** `support/prove_core0.py` now counts the SDK's blocking
  I2C transfers as unbounded waits, and names as flight roots the task ticks
  `hal_tasks_tick()` reaches through a function pointer (`pres_tick`,
  `pres_bringup_tick`, `buzzer_tick`), which a call graph cannot follow. Two
  of its roots, `flight_update` and `hal_watchdog_feed`, no longer existed
  and were skipped without a word; a root missing from the image now fails
  it, and `dispatch_state`, `watchdog_update`, `hal_tasks_tick` and
  `hal_log_sample` stand in. On HEAD's MK1A image it fails
  `pres_tick -> bmp280_read -> i2c_read_blocking`; on this change's images of
  all three boards it passes.

### DD-068: A Conversion A Flash Operation Ran Beside Is Not Used
- **Decision:** the flash layer advances `flash_op_seq` with every erase and
  program, before interrupts return. Each sensor notes it as a conversion
  starts and compares it as the conversion is read: the MS5607's handler for
  each code of its pair, the BMP280's cycle for its forced conversion. A
  changed count discards that code. A disturbed pressure is not fed on; a
  disturbed temperature is not put on the line. Each is counted
  (`pres_flashed` on `/api/status`) and traced ('F' a pressure, 'G' a
  temperature), and `support/pressure_trace.py` lets a discarded pressure
  explain its slot.
- **Why (G4-M):** the pad Mach flag that G4's uploads raised on the bench
  MK1B was the flash disturbing the sensor. Ten rounds of G4's loads on
  2.1.697, with every conversion traced: while `api_check.py` and the
  uploads wrote flash, the pressure's residual went from 9.4 Pa to 21-25 Pa,
  with readings up to 91 Pa off. The raw codes carry it -- D1's scatter 2.7
  times its idle, D2's up to 5.7 times -- so it is the conversion, not the
  arithmetic. Of the temperature reads an erase held off, 83 % were
  outliers. The UI check, which only reads flash, disturbed nothing. The
  worst short rate reached 0.0217 of p against the flag's 0.029. At the old
  10 ms loop the same disturbance over half the span is twice the rate:
  G4-M's 6 flags in 12 runs. The 20 ms loop hid it, with a quarter to spare,
  and did not fix it.
- **Why it matters in flight:** the flight log writes flash all flight
  (DD-035), and before the Mach flag's first release the short rate sets it
  from any sample.
- **What it costs:** a disturbed conversion is a missing sample, 20 ms. A
  gap needs 250 ms before any fit is suspect (SNS-PRES-11). Sensor loss is
  judged only in flight, where only the log writes.
- **The mechanism** is presumed electrical: the flash's program and erase
  current on the rail both parts share. The fix does not depend on it.
- **Hardware:** on the bench MK1B (2.1.698, ten G4 rounds traced) the
  discarded pressures scattered 49 Pa during uploads, twice the kept ones.
  The kept ones still scattered 24 Pa while `api_check.py` ran: the buzzer,
  in test mode. A beep code alone, no flash write, takes MK1B from 9 to
  31-38 Pa for as long as it plays; the flag's short rate then trips about
  once in 20 codes. MK1C shows none: 6-7 Pa beeping or not. The buzzer is
  left to the user (task M3).
- **Correction, 2026-09-28:** this entry and the task list first put the
  difference down to MK1B's buzzer hanging off a GPIO while MK1C's is
  switched by a MOSFET. The MK1B netlist says otherwise: BUZZER1 runs from
  VIN through Q1A, and GPIO16 drives only the gate, as MK1C's Q2 does. Why
  MK1B's buzzer disturbs its sensor and MK1C's does not is not established.
  Through G4 on all four boards (2.1.701): 23-38 conversions discarded
  each, no pressure rejects, no Mach flag.

### DD-067: The BMP280 At The Loop's Rate
- **Decision:** at the user's direction -- "S1 needs to make the BMP280
  match the loop rate" -- the BMP280 runs one forced conversion a loop.
  Detection leaves it asleep, x4 pressure and x1 temperature, no filter. Each
  loop, `bmp280_cycle()` takes the conversion the last loop commanded, then
  commands the next. It reads status and data in one burst; a conversion
  still measuring is a wait, and nothing is commanded over it. 50 pressures
  a second, each a conversion of its own.
- **Why:** in normal mode the part converted on its own clock, 11.5 ms
  typical, beating against the 20 ms loop. A read took whatever the
  registers held, and its stamp was a guess, 6 ms before the read. The
  datasheet recommends forced mode for "host-based synchronization"
  (section 3.6.2, page 16).
- **The stamp** is the command's time plus 7.5 ms: the middle of the
  pressure's measurement. Table 13 (page 18) gives the whole at 11.5 ms
  typical, 13.3 ms at most, and its rows add 2 ms typical a pressure
  oversample, so x4's pressure is the last 8 ms. A compile-time check keeps
  13.3 ms inside the period.
- **Also:** the transfers are bounded (DD-069), and a conversion a flash
  operation ran beside is discarded (DD-068).
- **Hardware (2.1.701, 2026-09-27):** MK1A traced for 60 s, 50.0 pressures
  a second, consistently good; the lag from stamp to read is 11.9-13.1 ms,
  where normal mode's guessed stamp showed a flat 5.8. G4 passes.
- **Supersedes** DD-063's option of a 100 Hz BMP280 (task S1).

### DD-066: A Pressure And A Temperature Every Loop
- **Decision:** at the user's question -- "Can we make the one shot collect
  a pressure and a temperature every time?" -- the one-shot converts a pair
  each loop: the pressure, then the temperature, commanded by the handler as
  it reads the pressure. The loop takes both at its next top and starts the
  next pair. 50 pressures a second where it was 45, each a loop after the
  last: no loop gives its slot to a temperature.
- **Why it fits:** each conversion at OSR 4096 takes up to 9.04 ms. With
  DD-036's 9.1 ms, and the commands and reads at 400 kHz, the pair is ready
  18.6 ms after the top, 1.4 ms before the next
  (`test_ms5607_pair_ready_before_the_next_loop` asks 0.5 ms). A compile-time
  check holds two conversions inside the period. At the reference template's
  100 kHz placeholder the pair would be ready 0.26 ms before the next top,
  and its note now says so.
- **The temperature at the pressure's time:** each pressure now sits between
  two temperatures, the last loop's 11 ms before it and its own 9 ms after.
  The line through the last four (SNS-PRES-12) now reaches back as well as
  ahead, no further back than its oldest reading. A die warming at 1 °C/s:
  0.70 Pa RMS, 1.00 Pa worst (`test_T9_temperature_at_the_pressures_time`);
  the pair's own temperature as read would cost 2.1 Pa RMS.
- **The step after a temperature is gone.** DD-063 found a pressure read
  just after a temperature conversion about 4 Pa high, relaxing over 90 ms:
  a sawtooth at the temperature's period. Every pressure now follows a
  temperature by the same 1.4 ms, so whatever the conversion leaves behind
  is the same in every sample, and a constant cancels against the pad. On
  MK1C the residual by samples since a temperature ran from +3.3 Pa to -2.5
  at 2.1.691; at 2.1.697 it folds flat, and the residual fell from 6.96 Pa
  to 6.43. The white noise remains (S2).
- **The trace:** each pair leaves a temperature record and a pressure
  record. `support/pressure_trace.py` had excused one missing slot at a
  temperature; one per pressure takes none, so it no longer does
  (`--selftest` covers both schedules).
- **Hardware (2.1.697, 2026-09-27, 60 s traces):** MK1C and both MK1Bs 50.0
  pressures a second, a temperature with each, intervals 19.98-21.32 ms, no
  gaps, no missed slots, no rejects, consistently good; noise 6.3-9.4 Pa.
  G4 passes on all four boards. MK1C counted one wait under G4's uploads: a
  sector erase in the slack holds interrupts off for tens of milliseconds,
  and a held handler finishes its pair late, as a held read did before. The
  filesystem is locked in flight (DD-058).
- **Supersedes** DD-051's "temperature is converted once in ten".

### DD-065: A 20 ms Loop
- **Decision:** at the user's direction -- "shift the loop rate to 50hz
  (20ms)" -- the main loop runs every 20 ms. The period lives in one place,
  `src/loop_period.h`, and everything that paces itself by it reads it there:
  the loop, MK1C's arm pump, the full log rate, and the host tests' model of
  the hardware, which step at it.
- **The sensors:** the MS5607 converts once a loop, the temperature once in
  ten, so 45 pressures a second where it was 90; each is read at the next
  loop, 15.5 ms after the middle of its conversion. The BMP280 was read every
  20 ms already. DD-063 measured the MS5607 at this period (MK1C, 45.0/s,
  clean, 6.7 Pa). The flight logic's host tests already sampled at 50 Hz.
- **MK1C's arm pump had to follow.** Its FIFO held 10 ms of pump -- four
  words and the one running, 2 ms each -- which carried it across a 10 ms
  loop. Across 20 ms it ran dry mid-precharge and U9 dropped out: seven of
  the MK1C firing tests failed at the new period. Each word now buys 5 ms, so
  the FIFO carries a loop and a quarter (`arm_pump.h` derives it). **What
  that costs, for review before the F1 bench fire:** a loop that stops now
  leaves the bus armed for up to about 35 ms (25 ms of queued pump, then U9's
  9.6 ms), where it was about 20. The fire comes a loop after the bus is up,
  about 20 ms after the command where it was about 10; a second event three
  loops after the first, still inside the 100 ms drain it does not wait for.
  The gate is held until the loop after 30 ms, up to 40 ms where it was up
  to 30; U9 has collapsed within 9.6 ms of the pump stopping, so the longer
  hold drives a dead bus. The tests' bounds are now stated in loops, with
  these reasons beside them, and F1's pass criteria follow.
- **MK1C's presence pulse is 20-21 ms** against DESIGN.md S3's 5-10 ms: the
  loop ends a phase no sooner than its next iteration. Task P1, decided as a
  timer one-shot, now matters twice as much.
- **Lua gets half the slices:** a grant a period, still capped at 5 ms, so a
  script's tick() runs 50 times a second where it ran 100. A script that
  counts ticks as time runs slow; one that reads the clock does not.
- **MK1B's presence stimulus** is on for a loop, 20 ms, and its reading after
  a fire lands about 40 ms after the pulse, inside the 100 ms verify window.
- **Supersedes** DD-063's "the 10 ms loop stays", which was the finding the
  user weighed.

### DD-064: Three Logging Plans
- **Decision:** at the user's direction -- "Default is 1hz logging with
  immediate logging for events. There are two higher rate plans. High rate
  1: 1hz logging, immediate logging for events, but +/-1s around event is
  full rate logging. High rate 2: full rate logging" -- `log_rate` replaces
  DD-062's `log_high_rate`: `1hz` (the default), `events` (High rate 1) and
  `full` (High rate 2) [FLT-LOG-07]. Every event row is kept at its own time
  under each.
- **The plan is the log's, not the flight's.** The flight code hands every
  sample to `hal_log_sample()`, and `src/log_plan.c` -- shared by the board's
  log writer and the test HAL -- decides what the log keeps. `src/hal.h` is
  unchanged.
- **`events` holds a second in RAM.** The second before an event is only
  known to matter once the event comes, and writing it then would put it
  after a 1-a-second row already written: the log would go back in time, and
  the Flight Data plot with it. So every row passes through a delay line of
  one second (128 rows, 3 KB), and each is decided as it leaves: an event row,
  within a second of an event, or due at the 1 Hz rate. The log's end flushes
  the line. The newest second is lost with power as the unflushed buffer
  already was. A Lua or mock text row does not pass through the line, so under
  `events` it can come before the second of samples before it.
- **The estimate** for `events` allows for ten events, each two seconds at
  the full rate. From what the bench boards reported: MK1C holds 4.3 days
  at `1hz` or `events` and 62 min at `full`; MK1B 11.3 h, 10.7 h and 7 min;
  MK1A, whose full rate is 50 Hz, 4.4 days and 2.1 h at `full`.
- **Only `full` replays** [DAT-08]; the log's header names the plan.
- **`log_high_rate`, a few hours old, is not carried over:** a `config.ini`
  that names it logs at `1hz`, like one that names `log_rate_hz`.

### DD-063: Every Pressure Conversion, Traced; 100 Hz Checked On Each Sensor
- **Decision:** at the user's direction -- "We need to verify that 100hz
  pressure data is valid. The sensor may operate at 100hz, but there could be
  duplicate data. Run a 100hz sensor test on all HW sensors ... We will drop
  the loop rate to 20ms if 10ms is too fast" -- the firmware keeps its last
  256 conversions (`src/pressure_trace.h`) and serves them at
  `/api/pressure/trace` [SNS-PRES-13]. `support/pressure_trace.py HOST`
  polls it and judges the sensor: rate and interval spread, stale reads,
  rejects, missed slots, lag and noise. A stale read is judged against what
  chance gives: noise repeats a code now and then, more often where the
  codes step by more than one -- 2 on the MS5607 at OSR 4096, 4 on the BMP280
  at x4, 8 at x2.
- **Measured 2026-09-27, 60 s each, idle:**

  | Board | Sensor, setting | Rate | Stale reads (chance) | Rejects | White noise | Verdict |
  |---|---|---|---|---|---|---|
  | MK1C | MS5607, 10 ms loop | 89.9/s | -- ; code repeats 12 (17.9) | 0 | 6.5 Pa | good |
  | MK1B bench | MS5607, 10 ms loop | 89.9/s | -- ; 15 (12.1) | 0 | 9.3 Pa | good |
  | MK1B second | MS5607, 10 ms loop | 89.9/s | -- ; 14 (15.0) | 0 | 7.8 Pa | good |
  | MK1C | MS5607, 20 ms loop (bench variant) | 45.0/s | -- ; 8 (8.7) | 0 | 6.7 Pa | good |
  | MK1A | BMP280 x4, read at 50 Hz (as shipped) | 49.9/s | 89 (88) | 0 | 2.1 Pa | good |
  | MK1A | BMP280 x4, read at 100 Hz (variant) | 99.9/s | **1149 (349)** | 0 | -- | **stale** |
  | MK1A | BMP280 x2, read at 100 Hz (variant) | 99.9/s | 298 (251) | 0 | 2.5 Pa | good, narrowly |

  The MS5607 answers every slot at 100 Hz: no zeros, no missed slots,
  intervals 9.96-10.01 ms and 20 ms across each temperature conversion.
- **The 10 ms loop stays.** Halving the MS5607's duty at a 20 ms loop left
  the noise where it was (6.7 Pa against 6.5), so the rate costs nothing, and
  half the samples would cost the fit. A 20 ms mode worth having would need
  its own one-shot sequence, as the user noted, and would still give no more
  than 100 Hz does.
- **The BMP280 cannot run x4 at 100 Hz:** it converts at about 85 Hz there,
  and 13% of reads took a conversion already read. At x2 it converts at
  125 Hz; the reads are fresh within what chance explains, narrowly (298
  stale against 251, about a one-in-700 excess), and the noise is the
  datasheet's (2.6 Pa, Table 8, page 15). MK1A stays at x4 and 50 Hz. A
  forced-mode read, one conversion commanded each loop as the MS5607's
  one-shot does, would make a 100 Hz BMP280 fresh by construction: task S1.
- **Two findings on the MS5607, both in its raw codes and so the sensor's
  own:** its white noise is 6.5-9.3 Pa against the datasheet's 2.4 Pa at
  OSR 4096 (MS5607-02BA03 page 4), the same at half the duty; and each
  pressure right after a temperature conversion reads about 4 Pa high,
  relaxing over the next 90 ms, the same on all three boards. Task S2.

### DD-062: A Row A Second By Default; Binary On Disk, CSV On Download
- **Decision:** at the user's direction (T8) -- "log at 1 row/sec (this is
  not replayable) but give the config a high-rate logging option ... Give
  them a maximum flight time estimator that updates with the log rate
  button", and "on-disk format should be binary. We can translate to csv
  on-the-fly when downloading":
  - **`log_high_rate`** (default false) replaces `log_rate_hz`. Off, the log
    takes a sample row a second; on, every sample: 100 rows a second on the
    MS5607 boards, 50 on MK1A's BMP280. Every event row is written at either
    rate [FLT-LOG-07]. A `config.ini` that still names `log_rate_hz` logs at
    the default, and its next save drops the key.
  - **`flight_log.bin`** holds binary records (`src/flight_log.h`): the magic,
    a header record, then 22-byte samples and text rows. `/api/flight.csv`
    renders them as the CSV the log used to be stored as, plus a
    `# Log rate:` line. The response is framed by Content-Length: the server
    counts the rendered length first, 4 KB of CSV a work unit, then streams
    the same rendering. A board with a log from before this change serves
    that CSV as it is; the erase removes both.
  - **`/api/log/space`** reports the room the next flight's log has -- free
    littlefs space plus the current log, which the next launch replaces,
    less four blocks for littlefs's metadata -- with the record size and the
    two rates. The Config tab's *High-rate logging* switch shows the longest
    flight that holds and updates as it is flipped [WEB-UI-06, WEB-API-12].
- **Why binary:** a sample is 22 bytes against about 38 as CSV, and the CSV
  is only for a person or a tool reading it, so it is made when it is read.
- **Not replayable at a row a second:** `pyro_sim --replay` refuses a log
  whose header says `1 row/s`, rather than deciding from a tenth of the
  readings [DAT-08].
- **The launch holdoff now also ends at 2 s** [FLT-LOG-05]. At a row a
  second the 4 KB RAM buffer takes three minutes to fill, and until the first
  write a power loss would lose the whole flight so far; 2 s is about what the
  full-rate log took to fill it.
- **A short write keeps only what was not written:** retried whole, the
  written part would appear twice, and one duplicated record misaligns every
  record after it.
- **The CSV rows are the old rows,** held to that by `test_flight_log.c`:
  the temperature is kept in tenths of a degree, so the rounding of a value
  exactly between two tenths can differ from the old `%.1f`.
- **Owed on the bench:** a flown log's download. The bench boards have none,
  and nothing on the bench can make one without a launch.

### DD-061: HTTP As Work Units, Run From The Slack Or By Core1
- **Decision:** at the user's direction -- "refactoring the http so it
  operates with work units that can be assigned to worker threads" -- the
  HTTP server is split in two. The transport (`http_server_transport()`, from
  `net_service()`) moves bytes between lwIP and each connection's rings and
  decides when to close, and runs no handler. Everything else is a unit: one
  `http_conn_service()` step on one connection, or a portable unit.
  `src/http_work.c` decides who runs each [WEB-HTTP-06, WEB-HTTP-07].
- **Why:** G4-L. Under G4's load MK1C's STAGE 1 peaked at 7.7 ms, of which
  7.4 ms was HTTP handlers: `http_server_service()` served every connection
  in one pass at the loop's head, before the flight work, and several
  `/api/status` renders in one pass pushed a loop past 10 ms (1-4 overruns a
  G4 run).
- **Core0 runs units only from the slack,** after STAGE 7, one at a time and
  in turn, and starts one only with `HTTP_UNIT_BUDGET_US` (2 ms) of the period
  left. The first of each period runs regardless, so HTTP still progresses on
  a loop with no slack; the overrun branch runs that one.
- **A portable unit touches nothing but its own connection,** and core1 may
  run it before its Lua slice. The only one is the `/api/status` render: on
  core0 `on_head` captures a `status_snap_t` in one pass, and `status_json()`,
  which links alone in its test, renders it. A render core0 had room for it
  runs at once; what it had no room for, the next grant hands to core1, which
  is charged `HTTP_WORKER_UNIT_US` (4 ms) per unit, leaving Lua at least
  1 ms. Core0 first because the slack is idle time: sending every render to
  core1 first measured 49 requests/s and a 20 ms median on one client,
  against 73/s and 13.5 ms core0-first.
- **Ownership is whole-connection and changes hands only on core0:** units
  are handed over before `lua_core1_dispatch()` bumps the grant and taken
  back when `lua_core1_flash_ok()` says core1 is idle. The transport skips a
  held connection entirely, so the rings need no cross-core protocol; a link
  that dies meanwhile only unlinks it, and the connection is released on its
  return. A unit core1 started and did not finish -- a kill mid-unit -- fails
  its connection.
- **`http_unit_vt` is named `*_vt`** so `support/prove_core0.py` folds its
  entries into core1's call graph; an image that links `http_work_run()`
  without it now fails.
- **Measured on the four bench boards (2.1.688), through G4:** 0 loop
  overruns on each, 0 flash refusals, 0 pressure rejects, every check passing,
  MK1C's Lua heartbeat climbing. STAGE 1 peaks at 1.9-2.1 ms (was 7.7 on
  MK1C), mostly lwIP; the transport's share is under 1 ms. Core0's costliest
  unit that writes no flash is 1.6 ms, core1's 3.8 ms, fetching from flash
  while core0 does. A unit that writes flash still runs to completion in the
  slack, as the old slack pass did: an upload's littlefs write takes 35-87 ms.
  One client polling `/api/status` on MK1C gets 73/s at a 13.5 ms median,
  where MK1A on the old path answered in 7.3 ms: work that ran at the loop's
  head now waits for the flight work.
- **`/api/status` keeps every key and its order, and gains five:**
  `http_units` and `http_unit_max_us` (core0, core1) after `stage1_parts_us`,
  whose third figure is now the transport; and after `recovery`, how the last
  boot ended -- `prev_watchdog`, true for a watchdog timeout rather than a
  requested reboot, and `prev_stage`/`prev_stage_ms`, the stage or crumb core0
  was in, read before anything restamps them. HTTP units have crumbs of their
  own (80-82), so a hang inside one names itself. The rocket's id and name are
  escaped: a quote in either had ended the JSON string [WEB-API-11].
- **Found on the bench:** the pad Mach flag (FLT-MACH-02) rises after G4's
  uploads, whose flash erases stretch the pressure readings' stamp lag to
  50-90 ms: 6 of 12 runs on the bench MK1B at 2.1.687-688 failed
  `api_check.py`'s "no Mach lock on the pad", and 2 of 4 on 2.1.683 -- an A/B
  on the same board, so it predates this change. Task G4-M. Once in those
  twelve runs the bench MK1B stopped answering HTTP for 40-60 s without
  resetting, cause unknown; it has not recurred in the fourteen G4 runs since,
  on the four boards. Task G4-N.

### DD-060: A Theory Of Operation Per Board; Structure In Place Of Comments
- **Decision:** at the user's direction -- "write a comprehensive theory of
  operation for each board type ... Reduce the comments by referencing the
  theory of operation ... The code functionality should be clear by the code
  structure and not rely upon comments" -- each board directory carries a
  `THEORY_OF_OPERATION.md` (MK1A, MK1B, MK1C, the reference template, and one
  for the simulator boards), and its code points into it with
  `See THEORY_OF_OPERATION.md "Heading"`. `support/trace_check.py` resolves
  such a pointer beside the file that makes it, and fails CI on a heading
  that is not there; it also checks that the functions a board's document
  names exist, and scans `.pio`, `.cmake` and `CMakeLists.txt` too.
- **What was the same on every board is written once:**
  - `src/hal_common/board_defaults.c`: the LED, the telemetry UART, the
    buzzer pad and `board_pyro_raw()`, as weak plain-GPIO defaults. The
    buzzer stays overridable, as `src/board_if.h` promised for a board with a
    piezo driver or a PWM slice behind it.
  - `src/pressure_single_sensor.c`: the bring-up for one sensor on one bus,
    which sensor chosen by the bus speed `board_pins.h` declares. MK1B, with
    two sensors on one SCL, keeps its own.
  - `src/bmp280_driver.c`, from the two boards' identical copies, on the
    board's `BOARD_I2C_INST`.
  - `src/board_support.h`: the deadline test, the median ADC read and the
    safe-output helper.
- **`board_early_init()` moves into each `pyro_board.c`.** Its one job is to
  put the pyro outputs down, and those are the pyro backend's pins;
  `hal_platform_init()` already silenced the buzzer before calling it. MK1B,
  which only silenced the buzzer there, now drives its enables and common low
  as the other boards do. What was left of `hal_board.c` -- picotool's pin
  names -- is `board_info.c`, and MK1B's now calls GPIO15 the shared low side,
  as the netlist does.
- **MK1C's `pyro_board.c` is split** by what each part is: the measurements
  (`pyro_measure.c`), the latched faults (`pyro_faults.c`), the firing
  sequence (`pyro_sequence.c`), and the `src/pyro.h` glue.
- **Names say what the code does:** the continuity phases are named for what
  they measure (`CHECK_PRESENCE`, `CHECK_SHORTS`), the fire steps for the
  step (`STEP_PRECHARGE`, `STEP_HOLD`), and the thresholds for what they
  separate (`PATH_TO_GROUND_MAX_COUNTS`, `NO_PATH_MIN_COUNTS`).
- **Behaviour is unchanged.** Every host suite passes as it did, and CI's
  format check follows the renamed files. `boards/sim_mk1b`, broken since
  MK1B's backend began asking which pads it owns, builds again with a
  `pin_store_owns()` of its own; `boards/sim_mk1c` gains the arm pump it
  lacked since DD-056.

### DD-059: MK1B Reads Continuity As MK1A Does
- **Found on the bench, 2026-09-27:** the second MK1B, which owns its pyros,
  reported `pyro1_short` and `pyro2_short` at 16 and 17 counts. MK1B's sense
  node is MK1A's (the netlist, `sim/plant/plant_mk1b.c`): 100k to 3V3, and
  PYRO_COMMON_EN the shared low-side gate. With the common on, a fitted
  igniter pulls the node to 0 counts -- and the firmware called anything
  under 50 a short, and only 50-3800 good. No healthy igniter could read
  good, so a MK1B with real igniters would never have deployed
  (PYR-SAFE-01). The plant's report had printed it: "MK1B 0 counts ...
  shorted=true good=false".
- **Decision:** MK1A's rule. Two readings a second: with the common off,
  after the idle second has recharged the node, a channel still low is
  shorted to ground; with it on, below 500 counts is an igniter (a 1k bad
  joint reads 41, a 10k leak 372), above 3000 open, and the raw count is
  reported. That is the presence-and-shorts check DD-055 asks for.
- **After a fire** the common stays on into the fresh reading, so there is
  no short reading then; the last one stands.
- `board_pyro_tests` models the node as the netlist has it: an igniter
  reads good, an empty connector open, a short to ground shorted, a 1k joint
  good with its count, and no read comes before its settle or recharge.
- **And then the part:** both MK1B builds fit U5 as AP2192AMPG-13, and the
  A variant discharges its outputs through about 100 ohm while disabled
  (DS32193 p.4, RDIS; note 6). That holds each sense node near 0 V whenever
  its channel is off, which is always during a check, so on the bench the
  second MK1B still reads both channels shorted (20 and 16 counts). No
  threshold can see past it: this rule is right for the netlist, and an MK1B
  senses continuity only once U5 is the base AP2192 (task B-U5).

### DD-058: The API Is Live In Flight; Only The Log Touches The Filesystem
- **Decision:** at the user's direction -- "Leave the USB and API live
  during flight. This will simplify testing." then "flight mode will lock
  out the filesystem except for logging" and "file writes must return a
  suitable error code" -- the in-flight refusal of every POST (DD-034,
  DD-057) is gone, and in its place the flight log holds the filesystem
  from launch until its tail is flushed:
  - the HAL's file calls return `HAL_FS_LOCKED` (-3), `hal_fs_open()` NULL;
  - a web request that needs a file -- an upload, a file GET, config, pins,
    beeps, serial, the log's erase -- is answered 423 Locked at its head,
    before any mount and before its body is read;
  - a transfer that took the filesystem before launch is reset on its next
    pass, and the log waits for it before it mounts (`hal_fs_enter()` counts
    every mount but the log's).
- **Why not simply later:** the conflict is a second mount, not the moment
  of a write. Every littlefs mount shares one set of buffers, and each keeps
  its own view of the metadata and the free blocks, so any mount beside the
  log's can corrupt it, whenever it writes. One mount for the whole firmware
  would let writes run in flight; that is a filesystem change of its own.
- **Why 423:** the file is locked, not the request malformed or the board
  in the wrong state. The 409s that remain mean the state: config and pins
  are taken on the pad only.
- **Live in flight:** status and every other GET, reboot, OTA (its own flash
  area, and kept out of a MK1C fire by DD-056), the beep audition and the
  Lua check. A reboot ends a chamber flight, and an OTA or a Lua compile
  stalls the loop; that is the tester's choice.
- **Resting on:** the API is reached only over USB, and a flying rocket has
  no PC on its cable. A board that carries the API over a radio needs the
  in-flight lock back.
- **The spent pad marker** waits for the log to let go of the filesystem;
  refused, it would never be written.

### DD-057: A Board On USB Is Not Flying: It Obeys A Reboot
- **Decision:** at the user's direction -- "If usb is attached you are not
  flying. Obey the USB reboot." -- `/api/reboot` is answered in a flight
  state while a USB host is on the port and test mode is off. Every other
  POST is still refused from launch to landing (DD-034).
- **Why:** the lock left a board stuck in a flight state -- a false launch
  that never arms, a sensor that died, N7 -- needing picotool or a power
  cycle (C5). A flying rocket has no PC on its cable, and the web server is
  reached only over USB.
- **Read live:** the flight machine holds the cable as it was at launch, so
  that a flight is never abandoned on the strength of it (USB-06).
  `flight_on_usb_now()` reads the cable as it is, for the reboot alone.
- **Test mode keeps the lock:** it flies on USB on purpose (USB-08), and a
  chamber flight must see what a real one does.
- **After the reboot:** a board booting on USB recovers cold (COLD_ON_USB),
  so it comes up on the pad, not back in the flight it was stuck in.
- **Superseded by DD-058:** every request is live in flight, the reboot
  included.

### DD-056: MK1C Fires: DESIGN.md 7.1 As Loop Steps
- **Decision:** at the user's direction ("Finish F1"), MK1C's `pyro_fire()`
  runs DESIGN.md 7.1 and IGNITER_OPERATION.md F0-F10, one step per loop
  iteration, with nothing waiting (DD-053):
  - **F0, the preconditions:** the channel read present on a tracking test
    since it last fired, no fault latched, the pack above a 3.0 V UVLO (an
    empty 1S cell, over U9's own 2.5 V lockout), and the bus not hot. Any
    one failing is a refusal, reported with its reason; `pyro_is_firing()`
    stays false and the flight records a refusal (PYR-FIRE-01). The tracking
    bias drops (invariant 1).
  - **F1-F2, arm and precharge:** the pump starts and each loop's re-check
    feeds it; the bus ramps at U9's 0.89 V/ms.
  - **F3, fire on the measured bus:** FIRE_x when SNS_BUS reaches 90 % of
    SNS_VBAT. Not there within 1.5 times the pump and the ramp (7.2): the
    pump stops, no gate is driven, and a fault latches.
  - **F6 with F3:** the pump stops when the gate is driven. The bulk
    capacitor requirement is dropped (the user, 2026-09-26), so U9's current
    limit is the pulse: about 4 A for the 9.6 ms its enable takes to bleed,
    which puts far more than M5's 15 mJ into a 1 ohm bridgewire. The misfire
    exposure is the same 9.6 ms, not more.
  - **F7-F8, the hold:** the gate is released once U9 is off and the bus is
    flat between two loops, or at 30 ms.
  - **F9, the drain:** the tracking test waits for a cold bus. A second
    channel may fire meanwhile (DESIGN.md 5.3), and a hot-bus fault is not
    judged for 100 ms; the bus's 1.1 uF bleeds on a 2 ms constant.
  - **F10, verify:** the first tracking test after the drain reports the
    fired channel open (fired) or still present (a misfire, live). Nothing
    latches: a misfire never inhibits the other channel (invariant 12).
    What follows on the ground, S8, is the flight code's.
- **The pump is fed once a loop.** Each word pushed is a 2 ms burst, so the
  FIFO's four and the one running carry the pump 10 ms, across one loop
  period. That is also all it can run past the last check: a loop that stops
  leaves the bus disarmed within about 20 ms. The PIO program and its state
  machine are claimed at boot, not at a fire.
- **One watchdog:** the loop's own. The pump does not need one to disarm --
  it stalls on its own when the loop stops feeding it -- and nothing
  disables the loop's watchdog any more.
- **No flash while a fire is in its sequence.** An erase stalls the loop for
  tens of milliseconds, longer than the pump coasts, and a stalled precharge
  times out and latches. `board_flash_ok()` (weak and true in
  `flash_window.c`) shuts the loop's window from the command to the gate's
  release, a few loop periods. MK1A and MK1B keep theirs: their pulse is a
  timed level that a stall only lengthens.
- **One tracking test is one sample** (invariant 8): the bus-short latch
  counts tests, not the loops that read a test, and a bus too low to judge
  presence by, under TRACK_BUS_MIN_COUNTS, is the short.
- **Not built:** ILM and FLT are not routed to the MCU, so the capacitance
  check of 7.2 and the FLT abort are absent; the precharge timeout covers a
  loaded bus. No decay profile is classified (DD-055: no live checks); F10's
  tracking test is the verdict.
- `board_pyro_mk1c_tests` runs it against the plant of the board as built,
  on 2S and 1S: a fire, the measured trigger, the pump's bounds, a stopped
  loop, an aborted precharge, each refusal, the verification, a misfire,
  both channels in turn, a high side stuck on, and the flash window. The
  bench fire into a dummy load is owed.

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
  `boards/mk1c/arm_pump.c`, for task F1.
- **Amended by DD-056:** the firing path is built.
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

