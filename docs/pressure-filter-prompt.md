# Implementation Prompt: Barometric Ground-Reference Tracking and Launch Detection for a Model Rocket Altimeter

## Role and context

You are implementing the pre-launch pressure processing for a model rocket flight computer. The altimeter uses a barometric pressure sensor to measure altitude above ground level (AGL), detect launch, and drive recovery events such as apogee and main-parachute deployment. A separate Mach lockout module suppresses baro-driven events while the rocket may be transonic; your code must expose the state that module needs but does not implement it.

While the rocket sits on the pad, the firmware must track slow atmospheric pressure drift and maintain a ground reference pressure. When launch is detected, tracking freezes and a ground reference from *before* the launch began becomes the fixed reference for all AGL thresholds. A rough ground reference is persisted to flash so that, if power is interrupted during flight, the firmware can resume with a usable AGL reference.

Before writing code, ask me for any of the following that are not already specified in the repository: the MCU and toolchain, the pressure sensor part number and its driver interface, whether an accelerometer is available (and its axis orientation and range), the flash layout available for persistence, whether the codebase uses fixed-point or floating-point math, and where the existing flight logging format is defined (its record layout, encoder, decoder or ground-side parser, and storage medium).

## Design principles

1. **Work in pressure, not altitude.** Filter and store pressure in pascals. Convert to AGL only when a height is needed, using the frozen ground pressure `P0`:
   `h_AGL = 44330 * (1 - pow(P / P0, 0.1903))` meters.
   Never compute AGL by subtracting two absolute altitudes derived from a standard sea-level pressure.
2. **Launch detection is always late.** The ground reference must come from a history buffer, not from the filter's value at the moment of detection.
3. **Require persistence, not single samples.** Every trigger (stability, launch, landing) must hold for a configured duration or sample count.
4. **Power can fail at any instant**, including during a flash write. Persistence must be torn-write safe.
5. **No dynamic allocation, bounded execution time per sample**, and no blocking flash erase during flight-critical states.

## Signal chain

Implement these stages as separate, individually testable units.

### Stage 1: Spike rejection
A running median of 3 or 5 raw pressure samples (configurable). This removes single-sample outliers from bus glitches, gusts at the vent ports, and handling.

### Stage 2: Fast smoothing ("current pressure")
A first-order exponential moving average on the median output:
`y += (x - y) * alpha`, with `alpha = Ts / tau` (or the exact `1 - exp(-Ts/tau)`).
In fixed point, use `y += (x - y) >> k` with enough fractional bits to avoid a dead band. Pad time constant `TAU_FAST` defaults to 1.0 s. This stage's output feeds stability detection, baro launch detection, and the ground tracker. In-flight state estimation (Kalman filter for altitude and velocity) is out of scope, but design the interfaces so it can consume Stage 1 output directly.

### Stage 3: Ground tracker (slow, gated EMA)
A slow EMA with `TAU_GROUND` defaulting to 20 s, fed from Stage 2.
- **Gating:** a sample updates the ground estimate only if `|P_fast - P_ground| <= GATE_PA` (default 40 Pa). Rejected samples are counted.
- **Gate recovery:** if samples are rejected continuously for `GATE_TIMEOUT_S` (default 30 s) while not launched, re-seed the ground estimate from Stage 2 and log the event. This prevents a real step change from locking the tracker out permanently.
- Tracking runs only in pre-launch states and freezes on launch detection.

### Stage 4: History ring buffer
Every `HISTORY_PERIOD_S` (default 0.5 s), push the current ground estimate with a timestamp into a ring buffer holding `HISTORY_DEPTH_S` (default 5 s) of entries. On launch detection, the frozen reference `P0` is the newest entry at least `LAUNCH_LOOKBACK_S` old (default 3 s, which must exceed worst-case detection latency plus margin). If the buffer does not yet hold an entry that old, use the oldest available entry and flag the reference as degraded.

## State machine

Implement an explicit state machine with logged transitions:

| State | Entry condition | Behavior |
|---|---|---|
| `BOOT` | Power-on | Run reset recovery (see below). Go to `WARMUP` or `FLIGHT_RESUMED`. |
| `WARMUP` | Fresh ground start | Filters run and the ground tracker seeds. Launch detection is disabled. |
| `PAD_STABLE` | Warm-up elapsed and stability criteria met | Write the first ROUGH record. Arm launch detection. Continue tracking and periodic refresh. |
| `LAUNCHED` | Launch detected | Freeze tracking, select `P0` from history, program the launch record. Hand off to flight logic. |
| `FLIGHT_RESUMED` | Reset recovery determined the rocket is in flight | Load the best stored reference. Report Mach lockout as active until the flight filter has re-converged. |
| `LANDED` | Landing detected | Program the landed flag. Stop flight events. |

Flight states after `LAUNCHED` (boost, coast, apogee, descent) belong to the flight logic. Your module must still detect landing so the persistence flags are cleared correctly.

## Stability criteria (entry to `PAD_STABLE`)

All of the following must hold:
- At least `WARMUP_MIN_S` (default 30 s) since power-on.
- Over a sliding window of `STABLE_WINDOW_S` (default 30 s), the standard deviation of Stage 2 output is at most `STABLE_STDDEV_PA` (default 4 Pa).
- The least-squares slope over the same window is at most `STABLE_SLOPE_PA_S` in magnitude (default 1.0 Pa/s).

Compute the window statistics incrementally with bounded memory, for example by decimating to 1 Hz before the window.

## Launch detection

Available only in `PAD_STABLE`. Support three configurations selected at build time or runtime:

1. **Accelerometer primary:** axial acceleration above `LAUNCH_ACCEL_G` (default 2.5 g) sustained for `LAUNCH_ACCEL_MS` (default 75 ms).
2. **Baro only:** AGL relative to the *current, unfrozen* ground estimate above `LAUNCH_BARO_M` (default 15 m) for `LAUNCH_BARO_N` consecutive samples (default 8), together with a positive vertical velocity estimate.
3. **Combined:** the accelerometer trigger requires baro confirmation (AGL above `CONFIRM_BARO_M`, default 5 m) within `CONFIRM_WINDOW_S` (default 2 s). Independently, a baro-only trigger with the stricter threshold `LAUNCH_BARO_FALLBACK_M` (default 25 m) fires on its own, so that a failed accelerometer cannot prevent launch detection.

Record the detection timestamp and which path fired.

## Flash persistence

### Record format
Each record contains a magic number, a format version, a monotonically increasing sequence number, a record type, the ground pressure in pascals (fixed point or float, documented), a timestamp, a flags field, and a CRC-32 over all preceding fields.

### ROUGH records (A/B slots)
- Two slots in separate erase sectors. Each write goes to the slot not holding the newest valid record, so a torn write always leaves the previous record intact.
- First write on entry to `PAD_STABLE`.
- Refresh every `ROUGH_REFRESH_S` (default 300 s) while in `PAD_STABLE`, but only if the ground estimate has moved by more than `ROUGH_REFRESH_DELTA_PA` (default 15 Pa) since the last write. This bounds flash wear.
- Sector erases may occur only in `WARMUP` or `PAD_STABLE`. If the platform stalls instruction fetch during erase (for example, execute-in-place flash on RP2040), perform the erase from RAM-resident code and account for the stall in the timing budget.

### Launch record (pre-erased page)
- Before entering `PAD_STABLE`, ensure a page is erased and reserved for the launch record.
- On launch detection, **program only** (no erase) the precise `P0` from the history buffer together with a launched flag. This must complete in well under 1 ms.
- The landed flag is programmed into a reserved field of the same pre-erased region so that no erase is needed after launch.

## Reset recovery (`BOOT`)

1. Read both ROUGH slots and the launch record. Discard any record with a bad magic, version, or CRC.
2. Take several pressure samples to get a current reading.
3. Treat the rocket as **in flight** if either of these holds:
   - a valid launch record exists with no landed flag, or
   - a valid ROUGH or launch record exists and current pressure indicates more than `RESUME_AGL_M` (default 40 m) above it.
4. If in flight, enter `FLIGHT_RESUMED`. Use the launch record's `P0` if it is valid, otherwise the newest valid ROUGH value, and flag which one was used.
5. Otherwise, treat this as a fresh ground start. Enter `WARMUP` and allow the new session's records to supersede the old ones. The first ROUGH write of the new session must invalidate any stale launch record.
6. If no valid record exists and pressure suggests flight is impossible to determine, enter `WARMUP` and log the ambiguity.

## Configuration

Collect every tunable listed above (all names in `UPPER_SNAKE_CASE`) into one configuration structure or header with units in each name or comment. Validate at startup that `LAUNCH_LOOKBACK_S < HISTORY_DEPTH_S` and that the lookback exceeds the worst-case baro detection latency for the slowest expected launch (use 3 g as the design floor, which takes about 1.2 s to reach 15 m).

## Logging integration

The flight computer already has a logging format. Your code must log through it rather than inventing a parallel log. Work through the following steps in order.

### Step 1: Inventory the existing format
Read the existing record layout, encoder, and decoder. Produce a mapping table with one row per quantity this module needs to log (listed below), showing whether an existing field already carries it, its units and resolution, and whether that resolution is adequate. Reuse existing fields wherever their meaning and resolution match. Never change the meaning, units, or scaling of an existing field.

### Step 2: Classify each missing quantity
Every quantity the existing format does not carry falls into one of two classes:

- **Flight-wide:** needed for the whole flight or on every sample, so it is required to reconstruct or replay the flight. It gets a **dedicated logging position**.
- **Event-specific:** meaningful only when a particular event occurs. It goes in the **Extra field** of that event's record.

### Step 3: Handle flight-wide quantities
For each missing flight-wide quantity, write a proposal containing the field name, type, units, scaling, position in the record, and the per-sample bandwidth cost. Present the proposals to me for approval **before** changing the record layout. Once approved, add the field, bump the log format version, and update the encoder, the decoder or parser, and any documentation together in the same change.

### Step 4: Handle event-specific quantities with an Extra field
If the existing format has no Extra field, extend it with one. Requirements:
- **Self-describing encoding:** a tag-length-value (TLV) sequence in which each entry has a tag ID, a length, and a value. A parser that does not recognize a tag must be able to skip it using the length.
- **Tag registry:** define every tag ID, its type, units, and scaling in one header shared by the encoder and decoder. Tag IDs are never reused or repurposed.
- **Bounded size:** Extra has a documented maximum length per record. The encoder must reject or truncate an oversized Extra cleanly and set a flag, never overrun the record.
- **Compatibility:** records without an Extra field, and log files written before the format change, must still decode correctly. Bump the log format version when Extra is introduced.

Adding the Extra field itself is a format change, so include it in the Step 3 proposal for approval.

### Quantities to log

The classification below is the default. Adjust it after the Step 1 inventory if the existing format suggests a better fit, and explain any change.

**Flight-wide (dedicated positions), every logged sample:**

| Quantity | Notes |
|---|---|
| Timestamp | Monotonic, from the same clock used by the state machine |
| Raw pressure (Pa) | Before any filtering, so filters can be re-run offline |
| Filtered pressure, Stage 2 (Pa) | |
| Ground reference (Pa) | The live ground estimate before launch and the frozen `P0` after launch |
| Sensor temperature | Needed to diagnose thermal drift |
| State machine state | Current state ID |
| Status flags | Bit field: gate currently rejecting, `P0` degraded, reference source (launch record or ROUGH), Mach lockout active, Extra truncated |

AGL is derived from pressure and the ground reference, so do not add a dedicated AGL field unless the existing format already has one.

**Event-specific (Extra field), logged once per event:**

| Event | Extra contents |
|---|---|
| State transition | Previous state, new state, reason code |
| Stability declared | Window standard deviation, window slope, time since power-on |
| Launch detected | Detection path (accel, baro, combined, baro fallback), detection latency estimate, selected `P0`, age of the history entry used, degraded flag |
| ROUGH record written | Slot, sequence number, pressure value, change since last write |
| Launch record programmed | Pressure value, program duration, success or failure |
| Gate re-seed | Old ground estimate, new ground estimate, duration of continuous rejection |
| Reset recovery | Validity of each stored record, decision (fresh start or flight resumed), reason, reference source chosen, AGL at resume |
| Landing detected | Detection criteria values, time since launch |
| Flash or persistence error | Operation, address or slot, error code |

### Timing and storage constraints
If the log shares flash with the persistence records, logging must never erase a sector in `LAUNCHED` or later states, and log writes must not delay the launch-record program. Buffer log records in RAM around launch detection if necessary. State the per-sample logging cost and confirm it fits the timing budget.

## Testing requirements

Build a host-side test harness that runs the same filter and state machine code on recorded or synthetic pressure data. Provide synthetic generators and tests for at least:

1. Sensor noise alone: the ground estimate converges and stability is declared within the expected time.
2. Weather drift of 0.5 hPa/hr and 2 hPa/hr over a 60-minute pad hold: ground tracking error stays within 1 m, and ROUGH refreshes occur as configured.
3. Gusts (short pressure excursions of 20–80 Pa) and single-sample spikes: no false launch, and the ground estimate is not disturbed beyond 0.5 m.
4. A sustained pressure step (simulating a weather change or relocating the rocket): gate recovery re-seeds the tracker.
5. Launches at 3 g, 10 g, and 30 g, each with and without an accelerometer: detection latency is measured, and the error of `P0` versus the true pre-launch pressure is under 0.5 m.
6. A handling event (a short accelerometer spike with no pressure change): no false launch in combined mode.
7. Power loss injected at every byte of a ROUGH write and of the launch-record program: recovery always yields a valid record or cleanly falls back.
8. Resets at the pad, during boost, at apogee, during descent, and after landing: recovery selects the correct state and reference.

9. Log round trip: every record type, including each Extra tag, encodes and decodes to the same values. A decoder built before the format change reads new logs without error by skipping unknown tags, and the new decoder reads old logs.
10. Offline replay: re-running the filters on logged raw pressure reproduces the logged filtered pressure and ground reference within numerical tolerance.

Report detection latency, `P0` error, and ground-tracking error for each scenario in the test output.

## Deliverables

- Source for the filter stages, ground tracker, history buffer, stability detector, launch detector, persistence layer, and state machine, each with a narrow interface.
- The configuration header.
- The logging field mapping table and the approved format-change proposal, followed by the updated encoder, decoder or parser, Extra tag registry, and format documentation.
- The host test harness and synthetic data generators.
- A short design note documenting the timing budget per sample, flash layout, and record format, together with any deviations from this specification and the reasons for them.
