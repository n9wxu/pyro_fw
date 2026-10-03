# Pyro Requirements

What the Pyro flight computers do, on every board, stated as behaviour.

Requirements are organised in three levels:
- **User needs** (UN): what the user wants to accomplish.
- **System requirements** (SYS): what the product does to meet a need.
- **Requirements**: specific, measurable and testable.

Each derived requirement traces to its parent with `← parent_id`.

## How to read this document

- **Behaviour, not mechanism.** A requirement can be verified from outside
  the firmware and met by any implementation. No processor, kernel, bus,
  interrupt or algorithm is dictated. The mechanisms chosen are recorded in
  `DECISIONS.md` and in each board's `boards/<name>/THEORY_OF_OPERATION.md`
  (DD-080).
- **General first, board beneath.** Every requirement applies to every
  board. Where a value is **board-declared**, each board states it, and
  section 17 lists what a board declares. A board may redefine a default or
  narrow a range. It may never withhold a decided fire.
- **‡** marks a value that is provisional until the modelling study
  (`docs/descent_speed_estimator.md`) or a bench measurement confirms it.
- **Withdrawn** identifiers are listed in Appendix A and are never reused.

### Terms

- **Enabled channel**: a pyro channel whose pads the pin assignment gives to
  the flight software and whose configured mode is other than none.
- **Flight**: from launch declared to landing declared.
- **Filtered state**: the system's estimate of the pressure, its rate and
  its acceleration (SNS-EST-01). Every flight comparison is made in pressure.
- **Descent speed**: the filtered descent rate, expressed as the speed the
  same body would have in the pad's air (FLT-AIR-01).
- **Pulse**: one energising of one channel.
- **Sample time**: time counted on the samples' own stamps (SNS-PRES-08).

---

## 1. Recovery Deployment

### User need
- **UN-1**: The user needs recovery devices deployed at the correct flight events to safely recover the rocket.

A safe flight carries two independent pyro systems. This unit prefers no
deployment to an early or uninformed one, and the second system covers the
flight this unit cannot (DD-081).

### System requirements
- **SYS-DEPLOY-01**: The system shall fire pyrotechnic charges at configurable flight events. ← UN-1
- **SYS-DEPLOY-02**: The system shall support two independent pyrotechnic channels. ← UN-1
- **SYS-DEPLOY-03**: The system shall prevent pyrotechnic firing before the rocket has left the launch rail. ← UN-1
- **SYS-DEPLOY-04**: A fire shall be decided only on measured evidence that its flight event has happened: never early, never on an estimate of where the rocket ought to be, and never on a timer standing in for a measurement. Samples that arrive are believed and the system decides on them; only the absence of samples suspends a decision. ← UN-1, DD-081
- **SYS-DEPLOY-05**: Once a fire is decided nothing shall withhold, abort or indefinitely delay the attempt, and the system shall go on attempting deployment for as long as the measured descent shows it has not worked. ← UN-1, DD-081
- **SYS-DEPLOY-06**: The system shall resume a flight in progress after any restart. ← UN-1, DD-086

### Flight phases
- **FLT-PHASE-01**: The system shall detect the transition from ground to powered flight. ← SYS-DEPLOY-01
- **FLT-PHASE-02**: The system shall detect apogee (peak altitude). ← SYS-DEPLOY-01
- **FLT-PHASE-03**: The system shall detect landing. ← SYS-DEPLOY-01
- **FLT-RT-01**: No network, script or storage activity shall delay a flight decision or a pulse by more than the board-declared bound, which shall not exceed 250 ms. ← SYS-DEPLOY-04, SYS-DEPLOY-05

### Launch
- **FLT-LAUNCH-02**: The system shall remain in PAD_IDLE while the rocket is at or below 100 feet above the ground reference. ← FLT-PHASE-01
- **FLT-LAUNCH-03**: T+0 shall be the time of the first sample of the rise more than 50 cm above the pad, not the moment of detection. ← FLT-PHASE-01
- **FLT-LAUNCH-04**: The system shall log a LAUNCH event at the transition. ← FLT-PHASE-01
- **FLT-LAUNCH-05**: The system shall stop the buzzer upon launch detection. ← FLT-PHASE-01
- **FLT-LAUNCH-07**: The system shall declare launch, and enter ASCENT, only when a height more than 100 feet above the ground reference and a vertical speed above 5 m/s have held together for 100 ms of sample time, and never while a USB host is attached (USB-01). ← FLT-PHASE-01

### Ground reference
- **GND-CAL-01**: The ground reference shall follow the ambient pressure on the pad, as its mean over the last 5 seconds. ← FLT-PHASE-01
- **GND-CAL-03**: A sample more than 50 Pa from the reference shall not move it, so a climbing rocket cannot drag it. ← GND-CAL-01
- **GND-CAL-04**: The reference shall freeze at launch to the pad's pressure from before T+0, not to the pressure at detection. ← GND-CAL-01
- **GND-CAL-05**: Altitude at launch detection shall report the height actually reached, not zero. ← GND-CAL-04
- **GND-CAL-06**: When every sample has been more than 50 Pa from the reference for 5 s while the board is still (under 1 m/s), the reference shall move to the current pressure, say so on telemetry, be counted on `/api/status`, and be recorded afresh for a resume (FLT-BROWN-01). ← GND-CAL-03
- **GND-CAL-07**: A reference frozen on less than a second of the pad shall be reported as degraded on `/api/status`. ← GND-CAL-04

### Arming and apogee
- **FLT-ASC-01**: The system shall track the peak of the flight during ascent. ← FLT-PHASE-02
- **FLT-ASC-02**: Every detector shall take its vertical speed from the filtered state (SNS-EST-01). ← FLT-PHASE-02
- **FLT-ASC-03**: The system shall report the thrust phase while the filtered acceleration is upward. The report shall end within 1 s of burnout. ← FLT-PHASE-01
- **FLT-ASC-04**: The system shall arm pyrotechnics when vertical speed drops below 10 m/s. ← PYR-SAFE-04
- **FLT-ASC-05**: The system shall log an ARMED event when pyrotechnics are armed. ← PYR-SAFE-04
- **FLT-ASC-06**: The system shall not arm pyrotechnics while vertical speed exceeds 10 m/s. ← PYR-SAFE-04
- **FLT-ASC-07**: The system shall not arm pyrotechnics unless a vertical speed above 10 m/s was measured during ASCENT. ← PYR-SAFE-04, SYS-DEPLOY-04
- **FLT-APO-01**: The system shall declare apogee from the filtered pressure passing its minimum: not before the true apogee, and within 1 s ‡ after it, at every height up to the board's height for proper operation (SNS-MAX-01). ← FLT-PHASE-02, DD-085
- **FLT-APO-02**: The system shall leave ASCENT for descent upon apogee detection. ← FLT-PHASE-02
- **FLT-APO-03**: The system shall log an APOGEE event at the transition. ← FLT-PHASE-02
- **FLT-APO-04**: The system shall not detect apogee before pyros are armed. ← FLT-PHASE-02, PYR-SAFE-04

### The Mach flag
Near and above Mach 1 the pressure a rocket's ports sense is not the air's,
and the error is smooth, so no filter removes it (`docs/mach_lockout.md`).

- **FLT-MACH-02**: The system shall set the Mach flag when the filtered climb rate exceeds 0.029·p per second (true Mach 0.62 to 0.76), from the first sample of the rise. ← FLT-PHASE-02
- **FLT-MACH-03**: The system shall release the flag only after the filtered state has, continuously for 1 s ‡, agreed with the readings, been climbing with a rate below 0.022·p per second, and been decelerating with p̈ ≥ 0.0009·p. The peak shall then restart at the current pressure. The release shall be reached at every height up to the board's height for proper operation. ← FLT-MACH-02, DD-085
- **FLT-MACH-04**: While the flag stands the system shall still find apogee: the filtered pressure passing its minimum and rising continuously for 3 s ‡ shall be declared apogee at any height, the pyrotechnics being armed first if they are not. No apogee rule shall wait for the pressure to return to the level the flag was set at. ← FLT-MACH-02, SYS-DEPLOY-05, DD-085
- **FLT-MACH-05**: The system shall not declare apogee from FLT-APO-01 while the flag stands. ← FLT-MACH-02
- **FLT-MACH-06**: No channel shall arm before the filtered pressure has been below 0.9965·p0 (about 30 m). A flight resumed into ASCENT shall start flagged, at the pressure it rejoined at. ← PYR-SAFE-04, FLT-MACH-02
- **FLT-MACH-07**: The reported peak shall be the height of the lowest filtered pressure outside the flag, marked a lower bound if the flag was released within 2 s of apogee or never. The web UI's Flight Data summary shall mark its apogee "at least" in the same cases. ← FLT-MACH-02

### Descent and landing
- **FLT-DESC-01**: The system shall determine the descent phase from the measured descent speed holding steady, not from which channel has been commanded. ← FLT-PHASE-02
- **FLT-DESC-02**: The system shall detect landing in every descent phase, so that a flight which deployed nothing still closes its flight log. ← FLT-PHASE-02
- **FLT-AIR-01**: The descent phases and the fire rules (PYR-REFIRE-01, FLT-EMRG-01) shall judge a descent speed as the pad's air would give it: the measured rate corrected to the atmosphere's own slope and scaled by sqrt(rho / rho_pad), with the 1976 US Standard Atmosphere's temperature at each pressure. A working canopy in thin air shall not be read as a failed one, and a canopy failing at any height shall still be. ← FLT-EMRG-01, FLT-DESC-01, DD-079
- **FLT-LAND-02**: The system shall require vertical speed below 2 m/s, for 1 s of sample time, for landing detection. ← FLT-PHASE-03
- **FLT-LAND-03**: The system shall require a height below 30 meters above the ground reference for landing detection, except under FLT-LAND-07. ← FLT-PHASE-03
- **FLT-LAND-04**: The system shall enter LANDED upon landing detection. ← FLT-PHASE-03
- **FLT-LAND-05**: The system shall log a LANDING event at the transition. ← FLT-PHASE-03
- **FLT-LAND-06**: The system shall remain in LANDED until the next start. ← FLT-PHASE-03
- **FLT-LAND-07**: The system shall detect landing at any height once descent has lasted the configured `landing_timeout` (default 60 s) and the rocket has been still: vertical speed below 2 m/s for 1 s of sample time, on a sensor that has not failed. ← FLT-PHASE-03

### Pyro firing modes
- **PYR-MODE-01**: The system shall support a DELAY mode that fires N seconds after apogee. ← SYS-DEPLOY-01
- **PYR-MODE-02**: The system shall support an AGL mode that fires when the rocket descends below a set height above the ground reference. ← SYS-DEPLOY-01
- **PYR-MODE-03**: The system shall support a FALLEN mode that fires when the rocket has descended a set distance from its peak. ← SYS-DEPLOY-01
- **PYR-MODE-04**: The system shall support a SPEED mode that fires when descent speed exceeds a set value. ← SYS-DEPLOY-01
- **PYR-MODE-05**: AGL, FALLEN and SPEED shall compare the filtered state, and FALLEN shall measure from the peak (FLT-MACH-07). DELAY shall count from the time the filtered rate crossed zero, or from the declaration when no crossing was seen. ← PYR-MODE-01, PYR-MODE-02, PYR-MODE-03, PYR-MODE-04
- **PYR-MODE-06**: A charge pressurising the bay, a rise of up to 5 kPa lasting up to 0.5 s ‡, shall not bring a pressure trigger forward: a trigger shall not act on a height lower than free fall from the state before the charge allows. ← PYR-MODE-05, SYS-DEPLOY-04

### Firing
- **PYR-SAFE-03**: No channel shall fire before its own trigger or the emergency fire (FLT-EMRG-01). After its first fire a channel shall fire again only under PYR-REFIRE-01 and FLT-EMRG-01. ← SYS-DEPLOY-02
- **PYR-SAFE-04**: The system shall not fire any pyro before apogee is declared. ← SYS-DEPLOY-03
- **PYR-DEPLOY-01**: The system shall allow both channels to deploy on a single flight event, so that a low flight can put out both canopies together. ← SYS-DEPLOY-02
- **PYR-DEPLOY-02**: The two channels shall never be energised together, in flight or in ground test. At least `fire_gap` of quiet shall separate the end of a pulse on one channel from the start of a pulse on the other. When pulses fall due together, a channel's first fire goes before the other's re-fire, and otherwise pyro 1 goes first. ← SYS-DEPLOY-02, DD-082
- **PYR-FIRE-01**: Every pulse shall be recorded, in the flight log and on `/api/status`, with what the board observed of it. No fire shall be refused. ← SYS-DEPLOY-05, DAT-04
- **PYR-REFIRE-01**: After a channel's first fire, while the descent speed exceeds that channel's re-fire speed (`pyro1_refire_speed`, `pyro2_refire_speed`), the channel shall be fired again every `refire_interval`. A re-fire speed of zero disables the channel's re-fire. ← SYS-DEPLOY-05, DD-082
- **FLT-EMRG-01**: At any time after apogee, when the descent speed reaches `emergency_fire_speed`, every enabled channel shall fire, whether or not it has fired before and whatever its trigger, and shall fire again every `refire_interval` until the speed falls below `emergency_fire_speed` or the flight lands. Zero disables the emergency fire. ← SYS-DEPLOY-05, DD-082
- **FLT-EMRG-04**: An emergency fire shall be recorded as one: an EMERGENCY_FIRE event in the flight log, and its flag and the count of re-fires on `/api/status`. ← FLT-EMRG-01, DAT-04
- **FLT-EMRG-05**: The re-fire and emergency rules shall act on the filtered state, which is the best knowledge the system has, and shall not wait on any further judgement of the data. ← FLT-EMRG-01, PYR-REFIRE-01, SYS-DEPLOY-04

### Resume
- **FLT-BROWN-01**: From 10 seconds of PAD_IDLE with no USB host attached (USB-01, USB-04), the system shall hold a record of the pad's ground pressure and sensor noise that survives a restart, so that nothing needs to be stored at launch. ← SYS-DEPLOY-06
- **FLT-BROWN-02**: After any restart, whatever its cause, the system shall resume the flight if and only if that record exists, the barometer shows the board above the recorded ground and moving, and no USB host is attached (USB-01). The level and the speed shall be judged across at least 0.5 s of readings, so that two bad readings cannot decide it. ← FLT-BROWN-01, DD-086
- **FLT-BROWN-03**: The system shall not treat a stationary board as airborne, whatever its apparent altitude. ← FLT-BROWN-02
- **FLT-BROWN-04**: All resume state shall be cleared when the flight lands and when a bench flight ends, so that no later start resumes against it. ← FLT-BROWN-02
- **FLT-BROWN-05**: `/api/status` shall say whether a start resumed a flight and, if not, why: no record, on USB, at ground level, or no sample in time. ← FLT-BROWN-02
- **FLT-BROWN-06**: A resumed flight shall measure altitude against the recorded ground, shall assume no channel has fired, and shall fire each enabled channel as soon as fresh sensor data meets its trigger. A flight resumed while descending takes apogee as passed. A DELAY shall count its full value from the resume. The emergency fire applies from the resume. ← FLT-BROWN-02, SYS-DEPLOY-05
- **FLT-BROWN-07**: A resume shall be recorded as an event in the flight log. ← FLT-BROWN-02, DAT-04

---

## 2. Pre-Flight Status

### User need
- **UN-2**: The user needs to verify the system is ready before placing the rocket on the pad.

### System requirements
- **SYS-STATUS-01**: The system shall indicate readiness and faults audibly without requiring a display. ← UN-2
- **SYS-STATUS-02**: The system shall verify pyrotechnic circuit integrity before flight. ← UN-2

### The pad announcement
- **BUZ-01**: The system shall announce one of four outcomes: OK to fly, pyro 1 fault, pyro 2 fault, general fault. ← SYS-STATUS-01
- **BUZ-02**: The announcement shall repeat on a configurable cadence, defaulting to every 5 s until launch, so that silence also means a fault. Not while a USB host is attached (USB-02). ← SYS-STATUS-01
- **BUZ-CODE-01**: The beep vocabulary shall be the set of actions available at the pad: OK to fly, check pyro 1, check pyro 2, general fault. ← SYS-STATUS-02
- **BUZ-CODE-02**: One outcome shall be announced at a time, by priority: general fault, then pyro 1, then pyro 2. A general fault is one that cannot be corrected at the rocket. ← BUZ-CODE-01, DD-083
- **BUZ-CODE-03**: The diagnosis shall be reported by name on `/api/status`, not encoded in the announcement. ← BUZ-CODE-01
- **BUZ-CODE-04**: Each outcome shall carry a stable key, a human-readable meaning, and a configurable sound. ← BUZ-CODE-01
- **BUZ-CODE-05**: A sound shall be a chirp, a steady tone, a beep count, or silence. ← BUZ-CODE-04
- **BUZ-CODE-06**: A beep count shall be 1 to 9 per group; a zero cannot be heard and a long count cannot be counted. ← BUZ-CODE-05
- **BUZ-CODE-07**: No two audible outcomes within a personality shall sound alike. ← BUZ-CODE-04
- **BUZ-CODE-08**: A personality that is wholly silent shall be refused. ← BUZ-CODE-04
- **BUZ-CODE-09**: The system shall hold three named personalities, one active. ← BUZ-CODE-04
- **BUZ-CODE-10**: A beep table that fails validation shall be rejected whole and the shipped personalities used. ← BUZ-CODE-04
- **BUZ-CODE-11**: The outcomes, their meanings and the personalities shall be served to the web interface so the firmware is the only place the vocabulary is written down. ← BUZ-CODE-04
- **BUZ-CODE-12**: A board with no stored beep table shall store the shipped personalities. ← BUZ-CODE-04
- **BUZ-CODE-13**: The shipped defaults shall follow the Eggtimer Rocketry convention: a rapid chirp for OK to fly, 5 beeps for a pyro 1 fault, 4 for a pyro 2 fault, 2 for a general fault. ← BUZ-CODE-01
- **BUZ-CODE-14**: A valid beep table that cannot be stored shall be reported as a storage failure, not as a validation failure. ← BUZ-CODE-10

### Pyro health
- **PYR-CONT-01**: The system shall check every enabled channel at least once per second during PAD_IDLE, for the faults the board can reliably detect. ← SYS-STATUS-02
- **PYR-CONT-02**: The system shall report each enabled channel as ready or as faulted, and shall name what the board measured on `/api/status`. ← SYS-STATUS-02
- **PYR-CONT-03**: The pad diagnosis and announcement shall be re-derived at every check, so that a fault which appears or clears on the pad changes the announcement without a power cycle. On USB the verdict is re-derived but not said (USB-02). ← PYR-CONT-01, FLT-BOOT-15
- **PYR-HEALTH-01**: A board that cannot detect a fault on a channel shall treat the channel as ready. No detected fault shall prevent a fire, in flight or in ground test. ← SYS-STATUS-02, SYS-DEPLOY-05, DD-083
- **PYR-HEALTH-02**: Only enabled channels shall count, for firing and for the pad verdict. The pin assignment is the source of truth: a channel whose pads belong to the script is not a pyro channel, and pyro configuration left for it is ignored. ← SYS-STATUS-02, DD-083

### Start-up
- **FLT-BOOT-01**: The system shall complete its start-up checks before entering PAD_IDLE. ← SYS-STATUS-01
- **FLT-BOOT-02**: The system shall read configuration from persistent storage during start-up. ← SYS-CFG-01
- **FLT-BOOT-05**: The system shall detect and initialise the pressure sensor during start-up. ← FLT-BOOT-01
- **FLT-BOOT-06**: The system shall initialise the pyrotechnic subsystem during start-up. ← FLT-BOOT-01
- **FLT-BOOT-07**: The system shall perform an initial pyro health check during start-up. ← SYS-STATUS-02
- **FLT-BOOT-08**: The system shall calibrate ground pressure from at least 10 readings, so that one bad reading cannot bias it. ← FLT-BOOT-01
- **FLT-BOOT-12**: The system shall enter a terminal FAULT state, and announce general fault, when no pressure sensor answers. ← FLT-BOOT-05
- **FLT-BOOT-13**: The system shall enter FAULT when calibration produces no samples within 10 seconds, rather than proceeding to PAD_IDLE. ← FLT-BOOT-05
- **FLT-BOOT-14**: The system shall enter FAULT, and announce general fault, when its storage cannot be used. ← FLT-BOOT-01
- **FLT-BOOT-15**: `/api/status` shall report every fault found on the pad. The buzzer announces the one of highest priority (BUZ-CODE-02). ← SYS-STATUS-02
- **FLT-RATE-01**: The system shall take at least 50 pressure readings a second from PAD_IDLE to landing. ← FLT-PHASE-01
- **FLT-RATE-05**: Every detector hold and dwell that measures the sensor shall run in sample time, so that lateness in processing changes no decision. ← SNS-PRES-08

---

## 3. Flight Data Recovery

### User need
- **UN-3**: The user needs to retrieve flight performance data after recovery.

### System requirements
- **SYS-DATA-01**: The system shall record flight data throughout the flight. ← UN-3
- **SYS-DATA-02**: The system shall export flight data in a standard format. ← UN-3
- **SYS-DATA-03**: The system shall announce maximum altitude audibly after landing. ← UN-3
- **SYS-DATA-04**: A board with a card and an accelerometer shall record the flight's motion at the accelerometer's rate on the card, with every pressure reading and the flight's state, and shall keep its files on the card. ← UN-3

### The flight log
- **DAT-02**: Each sample shall include: time, pressure, altitude, state, thrust flag, the raw reading it is centred on, the sensor temperature, and event. The time is the sample's own, since T+0 (SNS-PRES-08). ← SYS-DATA-01
- **DAT-03**: An event shall be recorded as a sample row at the event's own time. ← SYS-DATA-01
- **DAT-04**: The system shall log events: LAUNCH, ARMED, APOGEE, PYRO1_FIRE, PYRO2_FIRE, LANDING, and when they occur PYRO1/2_REFIRE, EMERGENCY_FIRE, PYRO1/2_NOPEN, PYRO1/2_FAULT, SENSOR_STUCK, SENSOR_LOST, RESUMED and the Mach flag's set and release. ← SYS-DATA-01
- **DAT-06**: The system shall keep flight data in persistent storage after landing and export it as CSV when it is read (WEB-API-06). ← SYS-DATA-02
- **DAT-07**: The CSV shall include a metadata header with configuration, flight summary and the rate it was logged at. ← SYS-DATA-02
- **DAT-08**: A flight log written with `log_rate=full` shall carry what is needed to replay the flight through the estimator and the detectors, and `pyro_sim --replay` shall do so and set the replay's events against the log's. It shall refuse a log that kept fewer samples. ← DAT-02
- **DAT-09**: Each board shall hold a flight log of the longest flight it supports, at each log rate, and shall declare that capacity. ← SYS-DATA-01, DD-084
- **DAT-10**: Any record that holds a pressure not yet converted to altitude shall hold the temperature the sensor measured with it. Converted outputs need none. ← SYS-DATA-01
- **FLT-LOG-06**: A flight that never lands shall keep its record: no more than the last second shall be lost. ← SYS-DATA-01
- **FLT-LOG-07**: The flight log shall keep, by `log_rate`: a sample row a second (`1hz`, the default); that and every sample within 1 s of each event (`events`); or every sample (`full`). It shall keep every event row at its own time under each, in time order. ← SYS-DATA-01
- **FLT-LOG-08**: Records the flight log could not keep shall be counted and reported on `/api/status`. ← SYS-DATA-01

### After landing
- **BUZ-03**: The system shall play an altitude beep-out sequence after landing, holding it while a USB host is attached and resuming it when the host goes (USB-02, USB-04). ← SYS-DATA-03
- **BUZ-04**: The altitude beep-out shall encode each digit of the peak altitude in configured units. ← BUZ-03
- **BUZ-05**: The digit 0 shall be encoded as 10 beeps. ← BUZ-04
- **BUZ-06**: The altitude beep-out shall repeat indefinitely. ← BUZ-03

---

## 4. Configuration

### User need
- **UN-4**: The user needs to configure the system for different rockets and flight profiles.

### System requirements
- **SYS-CFG-01**: The system shall store configuration persistently across power cycles. ← UN-4
- **SYS-CFG-02**: The system shall allow configuration changes without special tools. ← UN-4
- **SYS-CFG-03**: The system shall validate configuration against the limits of the sensor and of the board. ← UN-4

### The configuration file
- **CFG-01**: The system shall store configuration in an INI-format file on persistent storage. ← SYS-CFG-01
- **CFG-02**: The system shall parse the fields of the table below. ← CFG-01
- **CFG-03**: The system shall support unit settings: cm, m, ft. ← CFG-02
- **CFG-04**: The system shall support pyro mode settings: none (disabled), delay, agl, fallen, speed. A mode the system cannot name shall be stored as none. ← CFG-02
- **CFG-05**: The system shall create a default configuration if the config file is missing. ← SYS-CFG-01
- **CFG-06**: The system shall preserve existing config fields not present in a partial config file. ← CFG-02
- **CFG-07**: The system shall truncate id and name fields to 8 characters. ← CFG-02
- **CFG-08**: The system shall ignore unknown keys in the config file. ← CFG-02
- **CFG-09**: The system shall handle both CR+LF and LF line endings. ← CFG-01
- **CFG-10**: Configuration and pin changes shall take effect at start-up. Saving one stores the file; the running system, ground test mode included, goes on with the configuration it started with. ← SYS-CFG-01, DD-087
- **CFG-TABLE-02**: Every configuration field shall survive a save and a load unchanged. ← CFG-02
- **CFG-SUBSYS-01**: Every configuration key shall have an effect. ← UN-4

| Key | Meaning | Default |
|---|---|---|
| `id`, `name` | the rocket's identity, 8 characters each | PYRO001, MyRocket |
| `pyro1_mode`, `pyro1_value` | channel 1's trigger | delay, 0 |
| `pyro2_mode`, `pyro2_value` | channel 2's trigger | agl, 300 |
| `units` | cm, m or ft, for every height and speed entered or shown | m |
| `pyro1_refire_speed`, `pyro2_refire_speed` | PYR-REFIRE-01, in units per second; 0 disables | 0 ‡ |
| `emergency_fire_speed` | FLT-EMRG-01, in units per second; 0 disables | 0 ‡ |
| `refire_interval` | time between re-fires of one channel, ms | 1000, board may redefine |
| `fire_gap` | quiet time between pulses on different channels, ms | 3000, board may redefine |
| `log_rate` | 1hz, events or full | 1hz |
| `landing_timeout` | FLT-LAND-07, seconds | 60 |
| `lua_enabled`, `lua_baud`, `lua_pixels` | the script and its resources | false, 9600, 0 |

### Pyro values and the board
- **PYR-BOARD-01**: Each board shall declare the default and the permitted range of `refire_interval` and `fire_gap`, and may declare them for any other pyro field. Where a board declares none, the general values apply: `refire_interval` 500 to 10000 ms, `fire_gap` 1000 to 10000 ms ‡. ← SYS-CFG-03, DD-084
- **PYR-BOARD-02**: A configured value outside its permitted range shall be brought to the nearest permitted value, and the change reported on `/api/status` and in the web interface. The file shall not be rejected for it. ← SYS-CFG-03
- **PYR-BOARD-03**: The defaults and ranges in force shall be served to the web interface, so the firmware is the only place they are written down. ← SYS-CFG-02
- **PYR-BOARD-04**: A board constraint may change when and how often a channel is energised. It shall never withhold a fire the flight has decided. ← SYS-DEPLOY-05

### Pins
- **PIN-LABEL-01**: Every assignable pin shall carry the connector designator silkscreened on the board, and the web UI shall show it beside the pin's number. ← SYS-CFG-01
- **PIN-BUZZ-01**: The buzzer shall be assignable to any pad the board declares capable of driving one, defaulting to the board's own buzzer pad where it fits one. ← SYS-CFG-01
- **PIN-BUZZ-02**: A pad driving the buzzer shall be reserved against the script, and a pad holding a script role shall not be assignable as the buzzer. ← SYS-CFG-01

### The configuration editor
- **WEB-UI-02**: The web interface shall provide a guided configuration editor with input validation. Changing units shall convert the configured heights and speeds rather than reinterpret them, and a field with a length limit shall state it. ← SYS-CFG-02, SYS-CFG-03
- **WEB-UI-03**: The web interface shall warn when configuration has been saved but not applied. ← SYS-CFG-02, CFG-10

---

## 5. Pressure Measurement

### User need
- **UN-5**: The user needs accurate altitude measurement for pyro deployment and data recording.

### System requirements
- **SYS-ALT-01**: The system shall measure altitude using barometric pressure. ← UN-5
- **SYS-ALT-02**: The system shall operate with multiple pressure sensor types. ← UN-5

### Readings
- **SNS-PRES-01**: The system shall auto-detect the installed pressure sensor type. ← SYS-ALT-02
- **SNS-PRES-06**: A reading the sensor cannot produce -- a zero conversion, a failed transfer, or a value outside what the part can output -- shall be discarded and counted. A reading beyond the sensor's rated range, or taken at a temperature beyond its limits, is less accurate and shall still be used. ← SYS-ALT-01, DD-085
- **SNS-PRES-08**: Each sample shall carry the time its pressure was measured, to within 1 ms ‡, unaffected by lateness in processing or by storage activity. ← SYS-ALT-01
- **SNS-PRES-10**: A whole second of one reading, to the pascal, shall be taken as a failed sensor: a diagnostic bit, a SENSOR_STUCK event and a telemetry line. While it lasts, and for a second after, it counts as no data: nothing shall be decided on it. ← SYS-DEPLOY-04
- **SNS-PRES-11**: A gap of more than 250 ms between samples shall suspend decisions until a whole second of new samples exists. No sample for 0.5 s in flight shall be reported as a lost sensor: a diagnostic bit, a SENSOR_LOST event and a telemetry line. The flight carries on when samples return. ← SYS-DEPLOY-04
- **SNS-PRES-13**: The system shall keep the last 256 sensor conversions, with the time each was measured, when it was read, its raw and converted values and how it was classed, and serve them at `/api/pressure/trace`, so a sensor can be judged on the bench for stale reads, gaps and noise. ← SNS-PRES-08
- **SNS-PRES-14**: A reading the board knows its own storage activity disturbed shall not be used, and shall be counted on `/api/status`. ← SNS-PRES-06
- **SNS-REC-01**: The flight software shall not attempt to recover a failed sensor in flight. It shall record the failure and go on processing whatever samples arrive. ← SYS-DEPLOY-04, DD-085

### The estimate
- **SNS-EST-01**: Every flight decision shall be made on a filtered estimate of the pressure, its rate and its acceleration, formed from the raw readings: the filtered state. ← SYS-ALT-01, DD-085
- **SNS-EST-02**: One or two consecutive bad readings, of any value the sensor can produce, shall change no decision. A sustained run of readings is data and shall be followed. ← SNS-EST-01, SYS-DEPLOY-04
- **SNS-EST-03**: Noise alone shall cross no threshold. A board on a pad, in gusts of 30 Pa rms, shall declare no launch, and a rocket descending steadily 20 % ‡ below a fire rule's speed shall not trip it, at any sensor noise up to the board's declared figure. ← SNS-EST-01
- **SNS-EST-04**: The filtered state shall follow a real change: a descent speed that passes a fire rule's threshold shall be reported as past it within 2 s ‡. ← SNS-EST-01, SYS-DEPLOY-05
- **SNS-EST-05**: Every flight comparison shall be made in pressure. A height or a speed the operator set shall be converted to pressure once, against the pad's own pressure and the 1976 US Standard Atmosphere, and an AGL trigger shall act within 3 % ‡ of its set height in that atmosphere from any pad between sea level and 2000 m. ← SNS-EST-01, DD-085

### Altitude
- **SNS-ALT-01**: Altitude shall be reported relative to the launch pad, computed from the ground reference and the current pressure. It shall not be clamped: a point below the pad reads negative, and no ceiling is applied. ← SYS-ALT-01, DD-085
- **SNS-MAX-01**: Each board shall declare its sensor's pressure range and its height for proper operation, the greatest apogee at which FLT-APO-01 and FLT-MACH-03 hold. Above that height the system shall go on operating on the data it has; a reported altitude there may be inaccurate, and that shall not affect the flight. ← SYS-ALT-01, SYS-DEPLOY-05

---

## 6. Telemetry

### User need
- **UN-6**: The user needs real-time flight data transmitted for ground monitoring.

### System requirements
- **SYS-TEL-01**: The system shall transmit flight data via serial interface during flight. ← UN-6

### Requirements
- **TEL-01**: The system shall output telemetry in $PYRO NMEA sentence format. ← SYS-TEL-01
- **TEL-02**: Each telemetry sentence shall include an XOR checksum. ← SYS-TEL-01
- **TEL-03**: The system shall output one telemetry message a second in PAD_IDLE, in flight and in LANDED. ← SYS-TEL-01, DD-088
- **TEL-05**: The system shall not output telemetry during start-up or in FAULT. A faulted board shall instead send a `!FAULT` diagnostic line every 5 s, and a board in ground test mode the procedure's progress as diagnostic lines. ← SYS-TEL-01
- **TEL-06**: Each sentence shall include: sequence, state, altitude, speed, max altitude, pressure, flight time, flags. ← TEL-01
- **TEL-07**: The state field shall map: PAD_IDLE=0, ASCENT=1, FALLING=2, DROGUE_DESCENT=3, CHUTE_DESCENT=4, LANDED=5. The descent names label a measured rate band (FLT-DESC-01), not a channel. ← TEL-06
- **TEL-08**: The flags field shall encode: pyro 1 ready, pyro 2 ready, pyro 1 fired, pyro 2 fired, armed, apogee. ← TEL-06
- **TEL-09**: The sequence number shall increment with each sentence. ← TEL-01
- **TEL-10**: The thrust flag shall only be set during ASCENT when under thrust. ← TEL-06
- **TEL-11**: A flight event -- apogee, a fire, landing -- shall be queued and carried by the next telemetry message, so its report is at most 1 s late and none is lost. ← SYS-TEL-01, DD-088
- **TEL-12**: The telemetry port shall accept no commands. A user who needs another format assigns the serial pins to the script (LUA-PAD-02). ← SYS-TEL-01, DD-088

---

## 7. Pyro Protection

### User need
- **UN-7**: The user needs protection against pyrotechnic faults that could damage the system or cause unsafe conditions.

### System requirements
- **SYS-FAULT-01**: Each board shall protect its pyro drive against a load that would damage it. ← UN-7
- **SYS-FAULT-02**: The system shall detect the pyro fault conditions the board can sense. ← UN-7
- **SYS-FAULT-03**: The system shall notify the user of pyro fault conditions. ← UN-7

### Requirements
- **PYR-FAULT-01**: A board's protection may end a pulse. It shall never prevent the next attempt: protection that latches shall be cleared before each pulse. ← SYS-FAULT-01, SYS-DEPLOY-05
- **PYR-FAULT-02**: A board that can sense that its protection acted during a pulse shall record it. ← SYS-FAULT-02
- **PYR-FAULT-03**: A fault during a pulse shall be shown to the user in the flight log and on `/api/status`. ← SYS-FAULT-03
- **PYR-VERIFY-01**: A board that can sense it shall record, after a pulse, whether the channel opened. Nothing shall depend on that record. ← SYS-FAULT-02
- **PYR-ARM-01**: Software that stops running shall leave no channel energised or armed, within the board-declared time, which shall not exceed 50 ms. ← SYS-FAULT-01, SYS-DEPLOY-04
- **PYR-ARM-03**: A board whose firing path must be made ready before a pulse shall deliver the pulse when the path is ready or when its declared preparation time has passed, whichever is first. It shall never abandon the pulse. ← SYS-DEPLOY-05
- **PYR-ARM-05**: Storing data shall not shorten, lengthen or interrupt a pulse. When a pulse and a write fall due together the pulse goes first. ← FLT-RT-01
- **PYR-ARM-06**: A pulse that fails shall leave nothing latched that prevents or delays the other channel's pulse. ← PYR-DEPLOY-01

---

## 8. Web Interface & Network

### User need
- **UN-8**: The user needs to monitor, configure, and update the system from a computer without special software.

### System requirements
- **SYS-WEB-01**: The system shall provide a web interface accessible via USB connection. ← UN-8
- **SYS-WEB-02**: The system shall be discoverable on the network without manual IP configuration. ← UN-8

### Network
- **WEB-NET-01**: The system shall present a USB network interface to the host computer. ← SYS-WEB-01
- **WEB-NET-02**: The system shall serve DHCP and take the address 192.168.N.1, where N is the board's own subnet (WEB-NET-06). ← SYS-WEB-01
- **WEB-NET-03**: The system shall advertise its hostname via mDNS. ← SYS-WEB-02
- **WEB-NET-04**: The system shall advertise a DNS-SD service for automatic discovery. ← SYS-WEB-02
- **WEB-NET-05**: A frame the USB link cannot take yet shall be held and sent in order as soon as it can, not dropped; one shall be refused only when eight wait already or the host has let the device go. ← SYS-WEB-01
- **WEB-NET-06**: A board shall have a unique network address that survives restarts, without factory programming: one that has none shall draw one at random, with a subnet other than 0, 1 and 255, and keep it. `/api/status` shall say whether the address was drawn or assigned. ← SYS-WEB-02

### The API
- **WEB-API-01**: The system shall serve device status as JSON at `/api/status`. ← SYS-WEB-01
- **WEB-API-02**: The system shall serve the configuration file at `/api/config` (GET). ← SYS-WEB-01
- **WEB-API-03**: The system shall accept configuration updates at `/api/config` (POST) and write to persistent storage. ← SYS-WEB-01
- **WEB-API-04**: The system shall accept firmware updates at `/api/ota` (POST), answer before it restarts, and answer `Expect: 100-continue`. ← SYS-WEB-01
- **WEB-API-05**: The system shall trigger a device restart at `/api/reboot` (POST). ← SYS-WEB-01
- **WEB-API-06**: The system shall serve flight data as CSV at `/api/flight.csv`, framed by Content-Length. ← SYS-WEB-01
- **WEB-API-07**: All API responses shall include CORS headers. ← SYS-WEB-01
- **WEB-API-08**: The web API and USB shall stay live in flight. From launch until the flight's record is safe, only the flight's record shall be stored: any other storage access shall be refused, a web request with 423, and a web transfer that holds the storage when the flight starts shall be dropped. ← SYS-WEB-01
- **WEB-API-09**: The system shall erase the flight log on request at `/api/flight/erase` (POST), unless the log is being written. ← DAT-06
- **WEB-API-10**: A request for a file shall be refused with 423 while the flight log is being written. ← WEB-API-08
- **WEB-API-11**: `/api/status` shall be self-consistent, taken at one instant of the flight, shall keep its keys and their order, and shall be well-formed JSON whatever the configured rocket id and name contain. ← SYS-WEB-01
- **WEB-API-12**: The system shall report at `/api/log/space` the bytes the next flight's log has room for, the size of a sample record and the log rates, and refuse with 423 while the flight log holds the storage. ← SYS-WEB-01
- **WEB-API-13**: The system shall report at `/api/net` what the network has in use, has refused and has dropped, and the USB interface's mounts, unmounts, suspends and resumes, so an HTTP outage can be told apart on the bench. ← SYS-WEB-01

### HTTP
- **WEB-HTTP-01**: The HTTP server shall treat each connection as a byte stream: a request shall be answered the same however TCP divides it into segments, including a header block or body split at any byte and more than one request in a single segment. ← SYS-WEB-01
- **WEB-HTTP-02**: Every response shall be framed by Content-Length and carry Connection: close; one request is served per connection. ← SYS-WEB-01
- **WEB-HTTP-03**: The server shall read a request body only as fast as it consumes it, so that TCP flow control, not a refused segment, holds back a sender while the body waits for storage. ← SYS-WEB-01
- **WEB-HTTP-04**: The server shall refuse a malformed or oversized request with its HTTP status: 400 malformed, 405 unsupported method (with Allow), 411 no length, 413 body too large, 414 path too long, 431 header block too large. ← SYS-WEB-01

### The pages
- **WEB-UI-01**: The web interface shall display device status in the configured units. ← SYS-WEB-01
- **WEB-UI-04**: The web interface shall display flight summary data and allow CSV download. The summary shall come from the flight log alone, be re-read whenever it is shown, and name the flight it describes; flight time shall stop at the landing. ← SYS-WEB-01
- **WEB-UI-05**: The web interface shall support firmware upload and update checking. ← SYS-WEB-01
- **WEB-UI-06**: The Config tab shall offer the three logging plans, and shall estimate the longest flight the log holds under the plan chosen, updating as the choice changes. ← SYS-WEB-01

---

## 9. Firmware Update

### User need
- **UN-9**: The user needs to update firmware safely without risk of bricking the device.

### System requirements
- **SYS-OTA-01**: The system shall support firmware updates without physical access to the board. ← UN-9
- **SYS-OTA-02**: The system shall recover from a failed firmware update. ← UN-9

### Requirements
- **OTA-01**: The system shall support over-the-air firmware updates via HTTP. ← SYS-OTA-01
- **OTA-02**: The system shall write new firmware to an inactive slot while continuing to run. ← SYS-OTA-01
- **OTA-03**: The system shall automatically revert to the previous firmware if the new firmware does not reach normal operation on its first start. ← SYS-OTA-02
- **OTA-04**: A failed or interrupted update shall not affect the currently running firmware. ← SYS-OTA-02
- **OTA-05**: Firmware built for a different board shall not be kept: the system shall revert to the previous firmware. ← SYS-OTA-02

---

## 10. Scripting

### User need
- **UN-15**: The user needs to run auxiliary functions -- lights, cameras, payload, serial devices -- from a script, without changing the firmware.

### System requirements
- **SYS-LUA-01**: Every board shall run an operator-supplied Lua script with access to the flight's state and to the pads the pin assignment gives it. ← UN-15
- **SYS-LUA-02**: No script shall be able to block, prevent, or delay beyond its normal timing any behaviour of the board: starting up, the network and web interface, the pad announcement, logging, telemetry, or any flight function. ← UN-15, DD-089

### Requirements
- **LUA-ISO-01**: A script that loops, faults or exhausts its memory shall cost only itself: its error shall be reported and everything else on the board shall carry on within its normal timing. ← SYS-LUA-02
- **LUA-ISO-02**: A script shall read the flight's state and never write it. ← SYS-LUA-02
- **LUA-ISO-03**: A script shall have no means to fire an enabled pyro channel or to stop one firing. ← SYS-LUA-02
- **LUA-ISO-04**: A script shall have no access to files. It may add lines to the flight record. ← SYS-LUA-02
- **LUA-SAFE-01**: Whatever script is stored, the board shall start, reach its pad state and serve its web interface, so a bad script can always be replaced. A script that did nothing wrong shall not be left disabled. ← SYS-LUA-02
- **LUA-PAD-01**: Every pad shall have one owner, the flight software or the script, set by the pin assignment and fixed until the next start. ← SYS-LUA-01
- **LUA-PAD-02**: A script shall reach a resource by its assigned name and kind: output, input, serial or pixel. It has full control of the pads assigned to it, a pyro channel's included, and what it does with them is the operator's responsibility. A resource it was not given does not exist for it, and using one fails with a reported error. ← SYS-LUA-01
- **LUA-PAD-03**: Each board shall declare the resources it offers to scripts. ← SYS-LUA-01
- **LUA-RUN-01**: The enabled script shall always run: on the pad, in flight, in ground test mode and in a bench flight. ← SYS-LUA-01
- **LUA-MGT-01**: The web interface shall edit, check, save and remove the script. A script that fails its check shall not be saved. ← SYS-LUA-01
- **LUA-MGT-02**: A console shall show the script's output and errors, and whether the script is running or why it is not. ← SYS-LUA-01
- **LUA-IO-01**: The web UI shall export the script to a local file and import one back, so a program survives the loss of the storage that holds it. ← SYS-CFG-01
- **LUA-IO-02**: An imported program shall land in the editor and not on the device, so a mis-picked file costs nothing until it is saved. ← LUA-IO-01

---

## 11. Ground Test

### User need
- **UN-12**: The user needs to verify pyro circuits and system behavior on the ground without a computer.

### System requirements
- **SYS-TEST-01**: The system shall support a ground test of its pyro channels by a test switch. ← UN-12, DD-087

### Requirements
- **GND-TEST-05**: A board started with its ground test switch closed, and held closed for 0.5 s at the end of the start-up settle, shall enter ground test mode once its sensor and pyro health have been checked, and never the pad or a flight state. A board resuming a flight shall carry on flying, whatever the switch says. ← SYS-TEST-01
- **GND-TEST-06**: Ground test mode shall be announced by three long beeps and a pause, repeating. Nothing else shall take the buzzer while the mode lasts, the USB attach chirp included. ← SYS-TEST-01
- **GND-TEST-07**: The switch opened, once it has been held closed in ground test mode, shall start the procedure: a countdown at a count a second -- five fast beeps, then four, down to none -- and at zero pyro 1 fires; then a 3 s steady tone and a second countdown, and at its zero pyro 2 fires; then three long beeps, once, and silence. ← SYS-TEST-01
- **GND-TEST-08**: Only an enabled channel shall fire. With one channel enabled the tone and the second countdown shall be omitted; with none, the countdown shall lead to the all-clear. ← GND-TEST-07
- **GND-TEST-09**: The switch shall count as opened only after it has been held closed for 1 s in ground test mode, and a change of the switch only once it has held for 100 ms. ← GND-TEST-07
- **GND-TEST-10**: The switch closed again during a countdown or the tone shall stop the procedure before the next fire, and ground test mode shall be announced again. ← GND-TEST-07
- **GND-TEST-11**: Ground test mode shall last until the next start: it shall never detect a launch or open a flight log, and after the all-clear nothing more shall happen. ← PYR-SAFE-04
- **GND-TEST-12**: The ground test switch shall be assignable as none, a switch from one pad to ground, or a switch across two pads, and its pads shall be reserved against the script. The switch may connect the buzzer's pad to another pad; it shall not ground the buzzer. Across two pads the switch shall read closed only when the input pad follows the driven pad's state both ways; where the driven pad is the buzzer's, the input pad watches the buzzer's own pattern, and short pulses may be used to find the switch's position while the buzzer is silent. ← SYS-TEST-01, DD-087
- **GND-TEST-13**: A ground test fire shall be delivered on command: no health reading shall withhold it. ← SYS-TEST-01, SYS-DEPLOY-05

---

## 12. On USB

### User need
- **UN-13**: The user needs a board on the bench, plugged into a computer or a charger, to stay quiet and never behave as though it were flying.

### System requirements
- **SYS-USB-01**: While attached to USB, the system shall not detect a flight and shall not announce its status, unless the operator has put it in test mode. ← UN-13

### Requirements
- **USB-01**: While a USB host is attached and test mode is off, the system shall not declare launch, shall not resume a flight after a restart, and shall not record the pad for a resume. ← SYS-USB-01
- **USB-02**: While a USB host is attached and test mode is off, the system shall not announce the pad verdict, a general fault, or the altitude beep-out. The verdict and diagnosis stay on `/api/status`. This overrides BUZ-02, BUZ-03, FLT-BOOT-12 and FLT-BOOT-14 while attached. ← SYS-USB-01
- **USB-03**: On attach, and on leaving test mode while attached, the system shall play one OK-on-USB double chirp, and nothing more of its own until detached, except in ground test mode (GND-TEST-06). A sound an operator asks for from the web interface still plays. ← SYS-USB-01
- **USB-04**: On detach, the system shall resume what it would have been saying, and restart the 10 s dwell of FLT-BROWN-01. ← SYS-USB-01
- **USB-05**: From launch to landing, the attach state shall be ignored. ← SYS-USB-01
- **USB-07**: The system shall judge a host attached only on evidence that a host is present, so that every detection error leaves launch detection on. ← SYS-USB-01, SYS-DEPLOY-01
- **USB-08**: An operator-selected test mode shall make the system behave on USB as it does on battery: launch detection, deployment, the resume record and every announcement. It shall start off at every start, shall not change from launch to landing, shall be set from the web interface after a confirmation, and shall be reported on `/api/status`. ← SYS-USB-01, UN-12

---

## 13. Bench Flight

### User need
- **UN-14**: The user needs to see the board fly a flight, to any altitude it may reach, before it flies one.

### System requirements
- **SYS-SIM-01**: The system shall fly a scripted flight on the bench, to the top of its sensor's range, through its own flight software, logs and sensors, and fire nothing. ← UN-14

### Requirements
- **SIM-01**: A bench flight shall start only from PAD_IDLE with test mode on, and only a profile that can fly. ← SYS-SIM-01
- **SIM-02**: While one flies, the profile's pressure, from the 1976 US Standard Atmosphere, shall replace each reading after the pressure trace has recorded the sensor's, and the flight shall end once the profile and the flight software have both landed. ← SYS-SIM-01
- **SIM-03**: From the start of a bench flight until the board restarts, every fire shall be mocked and logged as one; a stop shall not give the channels back. ← SYS-SIM-01
- **SIM-04**: The bench flight shall offer profiles in which a canopy fails, so the re-fire and emergency rules can be seen on a board. ← SYS-SIM-01, PYR-REFIRE-01, FLT-EMRG-01

---

## 14. Card and High-Rate Log

Requirements on a board that carries a card and an accelerometer (SYS-DATA-04).
Log details are board constraints (DD-084).

- **SD-01**: A card that fails to come up shall leave the board on its internal storage, and shall be retried without a restart. Reading the accelerometer shall not wait on the card. ← SYS-DATA-04
- **SD-02**: While a card is mounted every file shall live on it, except the board's identity and the resume record. Each configuration file shall be copied into internal storage whenever the two differ, a blank card shall be seeded from internal storage, and a file the card lacks shall be read from internal storage. ← SYS-DATA-04
- **HR-01**: A high-rate log shall open with the second before launch, and launch shall wait on nothing to start it. ← SYS-DATA-04
- **HR-02**: From launch to landing, and on the bench on request, the high-rate log shall record every accelerometer and gyroscope reading delivered, every pressure and temperature conversion, and the flight's state ten times a second. ← SYS-DATA-04
- **HR-04**: Every record shall carry a check of its payload, a log shall be read to its last whole record, and a log a power cut left open shall be kept under a number at the next start. ← HR-02
- **HR-05**: A log the card cannot keep up with shall drop whole records and count them, and nothing the logger does shall delay a flight decision (FLT-RT-01). ← HR-02
- **HR-06**: A log whose file the card can no longer take shall go on in a new file from the next whole record, with the old file kept under a number; at most the one record the switch tears shall be lost. ← HR-02, HR-04

---

## 15. Power

### User need
- **UN-11**: The user needs the flight computer to operate on battery for extended pad time.

### System requirements
- **SYS-PWR-01**: The system shall minimise its power consumption wherever doing so affects no functional requirement. ← UN-11

Sizing the battery for the intended pad time is the user's task.

---

## 16. Project Requirements

Requirements on how the firmware is built, tested and written. They are not
product behaviour.

### User need
- **UN-10**: Contributors need to develop and test flight software without flight hardware.

### System requirements
- **SYS-PORT-01**: The flight software shall be testable on a host computer without hardware. ← UN-10
- **SYS-PORT-02**: The flight software shall be runnable in a browser-based simulation. ← UN-10
- **SYS-CFG-04**: Adding a configuration field shall require a change in one place. ← UN-10

### The hardware abstraction layer
- **HAL-01**: Flight logic source files shall contain no platform-specific code or conditional compilation. ← SYS-PORT-01
- **HAL-02**: All hardware interaction shall occur through a defined HAL interface. ← SYS-PORT-01
- **HAL-03**: The HAL interface shall support at least three implementations: hardware, test, simulation. ← SYS-PORT-01, SYS-PORT-02
- **HAL-04**: The same flight logic source files shall compile unchanged for all targets. ← HAL-01
- **HAL-05**: The HAL shall be the seam for testing: all flight code shall be testable against a mocked HAL, and porting to new hardware shall need a new HAL and no change to flight code. ← SYS-PORT-01, DD-090
- **HAL-06**: Each board's HAL shall be validated on hardware, independently of the flight software, with test equipment and HAL validation applications. ← SYS-PORT-01, DD-090

### Build
- **BLD-01**: The build system shall produce firmware for every supported board. ← SYS-PORT-01
- **BLD-02**: The build system shall produce host-compiled test executables. ← SYS-PORT-01
- **BLD-03**: The build system shall produce a host-compiled flight simulator. ← SYS-PORT-02
- **BLD-04**: The build system shall generate the firmware's version from the VERSION file. ← UN-10
- **BLD-05**: The build system shall support A/B firmware images for update. ← OTA-02
- **BLD-06**: The build system shall produce HAL validation applications, separate from the flight firmware. ← HAL-06

### Tests
- **TST-01**: Flight tests shall be black box: they shall drive the flight software through its published interfaces against a mocked HAL, and each shall trace to a requirement. ← SYS-PORT-01, DD-090
- **TST-02**: Integration tests shall verify complete flight sequences using recorded trajectory data. ← SYS-PORT-01
- **TST-03**: Closed-loop tests shall verify flight behavior with physics simulation feedback. ← SYS-PORT-01
- **TST-04**: Closed-loop tests shall cover all four pyro firing modes, and the re-fire and emergency rules with canopies that fail. ← PYR-MODE-01, PYR-MODE-02, PYR-MODE-03, PYR-MODE-04, PYR-REFIRE-01, FLT-EMRG-01
- **TST-05**: Closed-loop tests shall cover flights from 100 ft to above each sensor's height for proper operation, and shall assert best-effort behaviour above it. ← SYS-DEPLOY-01, SNS-MAX-01
- **TST-06**: Closed-loop tests shall verify that chute deployment reduces descent rate. ← SYS-DEPLOY-01
- **TST-07**: Web UI tests shall verify status display, configuration editing, and firmware update flows. ← SYS-WEB-01
- **TST-08**: All tests shall run in CI on every push to main. ← UN-10
- **TST-09**: Tests shall be traceable to requirements. ← UN-10

### Code
Libraries from outside this project are exempt from CODE-01 to CODE-10 and
are not rewritten or modified as part of this firmware.

- **CODE-01**: Code structure and naming shall be preferred to comments. ← UN-10, DD-090
- **CODE-02**: Comments shall carry requirement traceability. ← TST-09
- **CODE-03**: Comments shall explain to a subject matter expert the implementation decisions that the code's structure does not make clear. ← UN-10
- **CODE-04**: A clearly named function shall be preferred to a magic value, however well the value is named: a wait is a function that names what is waited for, not a delay by a constant. ← UN-10
- **CODE-05**: Comments shall never teach a topic. A pointer to a reference is right; an explanation of the subject is not. ← UN-10
- **CODE-06**: Functional units shall be isolated in files by the single responsibility principle. ← UN-10
- **CODE-07**: The code shall be SOLID and DRY. ← UN-10
- **CODE-08**: Tests shall be black box and traceable to requirements. ← TST-01
- **CODE-09**: All code shall be reachable through the published interfaces. Flight code shall have no dead code, no test-only functions, and no special test constructs that reach hidden functions. ← TST-01
- **CODE-10**: Pure functions with no internal state shall be preferred. ← UN-10

---

## 17. What a Board Declares

- **BRD-01**: Each board shall declare, in its theory of operation: the pyro faults it can reliably detect; its defaults and ranges for the pyro fields (PYR-BOARD-01); its pulse length and how its protection acts; its time to disarm when software stops (PYR-ARM-01); its sensor, the sensor's pressure range and noise, and its height for proper operation (SNS-MAX-01); its flight log capacity (DAT-09); its bound on the delay of a flight decision (FLT-RT-01); the resources it offers to scripts (LUA-PAD-03); and its connector labels (PIN-LABEL-01). ← SYS-CFG-03, DD-084
- **BRD-02**: A value a board declares shall be confirmed by that board's HAL validation (HAL-06). ← BRD-01

---

## Appendix A. Withdrawn

An identifier here is no longer a requirement. Where the text was a
mechanism, the design record named beside it still holds it.

### Replaced by the rules of the 2026-10 review
- **PYR-SAFE-01**: Withdrawn (DD-081). Pyro health is announced on the pad and gates no fire (PYR-HEALTH-01).
- **PYR-REFIRE-02**: Withdrawn (DD-082). A re-fire is decided on descent speed alone (PYR-REFIRE-01).
- **FLT-EMRG-02**: Withdrawn (DD-082). The emergency fire acts on descent speed at any time after apogee (FLT-EMRG-01).
- **FLT-EMRG-03**: Withdrawn (DD-082). The fire rules compare absolute speeds, so no judgement of a settled descent remains.
- **PYR-ARM-02**: Withdrawn (DD-081). No fire is refused (PYR-FIRE-01).
- **PYR-ALT-01**: Withdrawn (DD-085). No setting is clamped.
- **PYR-ALT-02**: Withdrawn (DD-083). A setting is not a pad fault and has no beep.
- **BUZ-STATUS-01**: Withdrawn (DD-083). The diagnosis is not in the sound (BUZ-CODE-03).
- **FLT-BOOT-11**: Withdrawn (DD-083). The order of the checks is replaced by the priority of the announcement (BUZ-CODE-02).
- **FLT-BOOT-16**: Withdrawn (DD-083). Replaced by the one definition of an enabled channel (PYR-HEALTH-02).
- **SNS-ALT-02**: Withdrawn (DD-085). Nothing is clamped to 8000 m.
- **SNS-ALT-03**: Withdrawn (DD-085). Altitude is relative to the pad and may be negative.
- **SNS-ALT-04**: Withdrawn (DD-085). It fenced in the two clamps.
- **GND-CAL-02**: Withdrawn (DD-085). Every operation is in pressure (SNS-EST-05).
- **FLT-LAND-01**: Withdrawn (DD-081). Its test rejected nothing at fifty samples a second; FLT-LAND-02 carries the stillness.
- **FLT-LAUNCH-01**: Withdrawn (DD-081). Folded into FLT-LAUNCH-07.
- **DAT-01**: Withdrawn (DD-084). Capacity is stated as flight duration (DAT-09).
- **TEL-04**: Withdrawn (DD-088). Folded into TEL-03: one message a second in every state.
- **TELEM-FMT-01**: Withdrawn (DD-088). The format is $PYRO; another format is a script on the serial pins (TEL-12).
- **TELEM-FMT-02**: Withdrawn (DD-088). Events are queued into the telemetry (TEL-11).
- **TELEM-FMT-03**: Withdrawn (DD-088).
- **GND-TEST-01**: Withdrawn (DD-087). The serial ground test commands are removed.
- **GND-TEST-02**: Withdrawn (DD-087).
- **GND-TEST-03**: Withdrawn (DD-087).
- **GND-TEST-04**: Withdrawn (DD-087).
- **USB-06**: Withdrawn (DD-037). No board can see a charger: VBUS reaches only the charger IC.
- **BUZ-07**: Withdrawn (DD-080). A duplicate of FLT-LAUNCH-05.
- **FLT-BOOT-03**: Withdrawn (DD-080). A duplicate of CFG-05.
- **SYS-PWR-02**: Withdrawn (DD-080). SYS-PWR-01 is the whole of the power requirement.

### Mechanism, held in the design record
- **SNS-PRES-02**: Withdrawn (DD-080). The low-pass filter; the estimator is DD-085's.
- **SNS-PRES-03**: Withdrawn (DD-080). The low-pass filter's first value.
- **SNS-PRES-05**: Withdrawn (DD-080). Conversion timing is each sensor driver's (DD-051, DD-067).
- **SNS-PRES-07**: Withdrawn (DD-080). The median of three (DD-040); the behaviour is SNS-EST-02.
- **SNS-PRES-09**: Withdrawn (DD-080). The quadratic fit (DD-048); the behaviour is SNS-EST-01 to SNS-EST-04.
- **SNS-PRES-12**: Withdrawn (DD-080). The MS5607's temperature compensation (DD-066).
- **FLT-RATE-02**: Withdrawn (DD-080). Samples are not delivered in batches.
- **FLT-RATE-03**: Withdrawn (DD-080). Slower sampling after landing is a design choice under SYS-PWR-01.
- **FLT-RATE-04**: Withdrawn (DD-080). Covered by HAL-02.
- **FLT-BOOT-04**: Withdrawn (DD-080). The sensor's power-on wait is the driver's.
- **FLT-BOOT-09**: Withdrawn (DD-080). The sensor's settling time is the driver's.
- **FLT-LOG-05**: Withdrawn (DD-080). When a board first writes its log after launch is a board log constraint (DD-084).
- **PYR-CONT-04**: Withdrawn (DD-080). MK1C's tracking test is in `boards/mk1c/THEORY_OF_OPERATION.md`.
- **PYR-ARM-04**: Withdrawn (DD-080). MK1C's pump and gate sequence is in `boards/mk1c/THEORY_OF_OPERATION.md` (DD-056).
- **WEB-HTTP-05**: Withdrawn (DD-080). Where the HTTP work runs (DD-073).
- **WEB-HTTP-06**: Withdrawn (DD-080). Where the HTTP work runs (DD-073).
- **RTOS-01**: Withdrawn (DD-080). The task model (DD-073); the behaviour is FLT-RT-01.
- **RTOS-02**: Withdrawn (DD-080). How a flash operation is made safe (DD-074); the behaviour is FLT-RT-01.
- **PWR-SAMPLE-01**: Withdrawn (DD-080).
- **PWR-SAMPLE-02**: Withdrawn (DD-080).
- **PWR-TELEM-01**: Withdrawn (DD-080).
- **PWR-TELEM-02**: Withdrawn (DD-080).
- **PWR-TELEM-03**: Withdrawn (DD-080).
- **PWR-BUZZ-01**: Withdrawn (DD-080).
- **PWR-BUZZ-02**: Withdrawn (DD-080).
- **PWR-BUZZ-03**: Withdrawn (DD-080).
- **PWR-USB-01**: Withdrawn (DD-080).
- **PWR-SLEEP-01**: Withdrawn (DD-080).
- **PWR-WAIT-01**: Withdrawn (DD-080). The no-sleep rule is a design rule (DD-053) with its own check.
- **PWR-WAIT-02**: Withdrawn (DD-080). Bounded bus transfers (DD-069).
- **PWR-LOG-01**: Withdrawn (DD-080). The behaviour is FLT-LOG-06 and FLT-RT-01.
- **PWR-LOG-02**: Withdrawn (DD-080).
- **PWR-LOG-03**: Withdrawn (DD-080).
- **PWR-LOG-04**: Withdrawn (DD-080).
- **CFG-TABLE-01**: Withdrawn (DD-080). The field table is how SYS-CFG-04 is met.
- **HR-03**: Withdrawn (DD-080). The card's write size (DD-077).

### Withdrawn before the review
- **PYR-SAFE-02**: Withdrawn (DD-021). Both channels may deploy on one event (PYR-DEPLOY-01).
- **FLT-LAUNCH-06**: Withdrawn (DD-016). There is no separate gain-within-a-window test.
- **FLT-APO-05**: Withdrawn (DD-022). No timer may force apogee.
- **FLT-APO-06**: Withdrawn (DD-022).
- **FLT-MACH-01**: Withdrawn (DD-049). FLT-MACH-02..07 replace it.
- **SNS-PRES-04**: Withdrawn (DD-044).
- **WEB-HTTP-07**: Withdrawn (DD-073).
