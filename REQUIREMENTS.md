# Pyro MK1B Requirements

Requirements are organized in four levels:
- **L1 — User Needs**: What the user wants to accomplish
- **L2 — System Requirements**: What the product does to meet user needs
- **L3 — Subsystem Requirements**: Derived from system requirements
- **L4 — Implementation Requirements**: Specific, measurable, testable criteria

Each derived requirement traces to its parent with `← parent_id`.

---

## 1. Recovery Deployment

### L1 User Need
- **UN-1**: The user needs recovery devices deployed at the correct flight events to safely recover the rocket.

### L2 System Requirements
- **SYS-DEPLOY-01**: The system shall fire pyrotechnic charges at configurable flight events. ← UN-1
- **SYS-DEPLOY-02**: The system shall support two independent pyrotechnic channels. ← UN-1
- **SYS-DEPLOY-03**: The system shall prevent pyrotechnic firing before the rocket has left the launch rail. ← UN-1

### L3 Subsystem Requirements

#### Flight Phase Detection
- **FLT-PHASE-01**: The system shall detect the transition from ground to powered flight. ← SYS-DEPLOY-01
- **FLT-PHASE-02**: The system shall detect apogee (peak altitude). ← SYS-DEPLOY-01
- **FLT-PHASE-03**: The system shall detect landing. ← SYS-DEPLOY-01

#### Pyro Firing Modes
- **PYR-MODE-01**: The system shall support a DELAY mode that fires N seconds after apogee. ← SYS-DEPLOY-01
- **PYR-MODE-02**: The system shall support an AGL mode that fires when altitude drops below a threshold. ← SYS-DEPLOY-01
- **PYR-MODE-03**: The system shall support a FALLEN mode that fires when altitude drops a specified distance from maximum. ← SYS-DEPLOY-01
- **PYR-MODE-04**: The system shall support a SPEED mode that fires when descent speed exceeds a threshold. ← SYS-DEPLOY-01
- **PYR-MODE-05**: AGL and FALLEN triggers shall compare the altitude corrected for the pressure filter's lag (rate × time constant), so that a trigger fires at its configured altitude on a fast descent. ← PYR-MODE-02, PYR-MODE-03

#### Firing Safety
- **PYR-SAFE-01**: The system shall not fire a channel that has no continuity. ← SYS-DEPLOY-03
- **PYR-SAFE-02**: Withdrawn (DD-021). Both channels may deploy on one event (PYR-DEPLOY-01).
- **PYR-DEPLOY-01**: The system shall allow both channels to deploy on a single flight event, so that a low flight can put out drogue and main together. ← SYS-DEPLOY-02
- **PYR-DEPLOY-02**: The system shall not energise both channels at the same instant, whether the fire comes from the flight or from a ground test. This is a current limit, not a sequencing rule: both igniters share one common element, and on a PTC-protected board the combined draw can trip it and fire neither. ← SYS-DEPLOY-02
- **PYR-FIRE-01**: The system shall record a channel as fired only when the board energised it. A fire command the board refuses shall be recorded as a refusal, in the flight log and on `/api/status`, and shall not be repeated. ← SYS-DEPLOY-01, DAT-04
- **PYR-SAFE-03**: The system shall fire each channel at most once per flight (except re-fire). ← SYS-DEPLOY-02
- **PYR-SAFE-04**: The system shall not fire any pyro before apogee is detected. ← SYS-DEPLOY-03

### L4 Implementation Requirements

#### Launch Detection
- **FLT-LAUNCH-01**: The system shall transition to ASCENT when filtered altitude exceeds 100 feet (3048 cm) above the ground reference, and never while a USB host is attached (USB-01). ← FLT-PHASE-01
- **FLT-LAUNCH-02**: The system shall remain in PAD_IDLE when altitude is at or below 100 feet. ← FLT-PHASE-01
- **FLT-LAUNCH-03**: The system shall record launch time as the timestamp of the first sample above 50 cm since the last sample at or below it. ← FLT-PHASE-01
- **FLT-LAUNCH-04**: The system shall log a LAUNCH event at the transition. ← FLT-PHASE-01
- **FLT-LAUNCH-05**: The system shall stop the buzzer upon launch detection. ← FLT-PHASE-01
- **FLT-LAUNCH-06**: Withdrawn. The launch height is FLT-LAUNCH-01's 100 feet; there is no separate gain-within-a-window test (DD-016).
- **FLT-LAUNCH-07**: The system shall declare launch only when altitude above 100 feet and vertical speed above 5 m/s have held together for 100 ms of sample time, so that two bad readings in a row cannot. ← FLT-PHASE-01

#### Ground Reference
- **GND-CAL-01**: The ground reference shall be a 5-second rolling mean of the filtered pressure. ← FLT-PHASE-01
- **GND-CAL-02**: The reference shall average PRESSURE, not altitude, so that the altitude clamp at zero introduces no bias. ← GND-CAL-01
- **GND-CAL-03**: A sample more than 50 Pa from the reference shall not be averaged into it, so a climbing rocket cannot drag it upward. ← GND-CAL-01
- **GND-CAL-04**: The reference shall freeze at launch to the mean of the pad before T+0 -- the blocks of GND-CAL-01's mean that ended before the first sample of the rise -- rather than being snapped to the pressure at detection. ← GND-CAL-01
- **GND-CAL-05**: Altitude at launch detection shall report the height actually reached, not zero. ← GND-CAL-04
- **GND-CAL-07**: A reference frozen on less than a second of the pad shall be reported as degraded on /api/status. ← GND-CAL-04

#### Apogee Detection
- **FLT-APO-01**: The system shall detect apogee when vertical speed has been at or below zero for 60 ms of sample time while pyros are armed. ← FLT-PHASE-02
- **FLT-APO-02**: The system shall transition from ASCENT to DESCENT upon apogee detection. ← FLT-PHASE-02
- **FLT-APO-03**: The system shall log an APOGEE event at the transition. ← FLT-PHASE-02
- **FLT-APO-04**: The system shall not detect apogee before pyros are armed. ← FLT-PHASE-02, PYR-SAFE-04
- **FLT-APO-05**: Withdrawn (DD-022). No timer may force apogee.
- **FLT-APO-06**: Withdrawn (DD-022).

#### Pyro Arming
- **FLT-ASC-01**: The system shall track maximum altitude during ascent. ← FLT-PHASE-02
- **FLT-ASC-02**: The system shall compute vertical speed from consecutive altitude samples. ← FLT-PHASE-02
- **FLT-ASC-03**: The system shall detect thrust phase when vertical speed is increasing. ← FLT-PHASE-01
- **FLT-ASC-04**: The system shall arm pyrotechnics when vertical speed drops below 10 m/s. ← PYR-SAFE-04
- **FLT-ASC-05**: The system shall log an ARMED event when pyrotechnics are armed. ← PYR-SAFE-04
- **FLT-ASC-06**: The system shall not arm pyrotechnics while vertical speed exceeds 10 m/s. ← PYR-SAFE-04
- **FLT-ASC-07**: The system shall not arm pyrotechnics unless maximum vertical speed during ASCENT exceeded 20 m/s. ← PYR-SAFE-04

#### Landing Detection
- **FLT-LAND-01**: The system shall detect landing when altitude change is less than 1 meter between consecutive samples for at least 1 second. ← FLT-PHASE-03
- **FLT-LAND-02**: The system shall require vertical speed below 2 m/s for landing detection. ← FLT-PHASE-03
- **FLT-LAND-03**: The system shall require altitude below 30 meters AGL for landing detection. ← FLT-PHASE-03
- **FLT-LAND-04**: The system shall transition from DESCENT to LANDED upon landing detection. ← FLT-PHASE-03
- **FLT-LAND-05**: The system shall log a LANDING event at the transition. ← FLT-PHASE-03
- **FLT-LAND-06**: The system shall remain in LANDED state permanently after landing. ← FLT-PHASE-03
- **FLT-LAND-07**: The system shall detect landing if descent has lasted 60 seconds and vertical speed is below 5 m/s, regardless of AGL altitude. ← FLT-PHASE-03

#### Pyro Re-fire
- **PYR-REFIRE-01**: The system shall re-fire the drogue channel once, 2 seconds after the initial fire, if the descent rate has not steadied under a canopy and the channel's post-fire continuity check shows it never opened. The retry is limited to one attempt per flight. ← SYS-DEPLOY-01
- **PYR-REFIRE-02**: The system shall not re-fire a channel whose post-fire continuity check shows it opened. An opened channel fired its charge, so the canopy failed mechanically and a second attempt cannot help. ← SYS-DEPLOY-01
- **FLT-EMRG-01**: When the drogue has been commanded (fired or refused) and, once it has had 2 seconds to deploy, the rocket descends faster than 35 m/s without being slowed for 1 second, the system shall deploy the main early, overriding its configured trigger. ← SYS-DEPLOY-01
- **FLT-EMRG-02**: The emergency ladder shall not act on a descent rate alone. A rocket in free fall toward a trigger it has not yet reached is following the flight plan, however fast it is descending. ← SYS-DEPLOY-01
- **FLT-EMRG-03**: The emergency ladder shall not act on the absence of a settled descent. A drogue opened at apogee is still accelerating toward its terminal rate for seconds, so "not yet settled" is true of a working drogue. ← FLT-EMRG-01
- **FLT-EMRG-04**: An emergency deployment shall be recorded as one: a MAIN_FORCED event in the flight log, and `main_forced` and the retry count on `/api/status`. ← FLT-EMRG-01, DAT-04
- **FLT-BROWN-01**: The system shall write a pad marker recording the ground pressure after 10 seconds of PAD_IDLE with no USB host attached (USB-01, USB-04), so that nothing needs to be written at launch. ← SYS-DEPLOY-01
- **FLT-BROWN-02**: After a power event, the system shall recover the ground reference from the pad marker rather than recalibrating, if and only if the barometer shows it is above the recorded ground AND moving, AND no USB host is attached (USB-01). The level and the speed shall be medians of the readings since power-on, the speed measured across at least 0.5 s, so that two bad readings cannot decide it. ← FLT-BROWN-01
- **FLT-BROWN-03**: The system shall not treat a stationary board as airborne, whatever its apparent altitude. ← FLT-BROWN-02
- **FLT-BROWN-04**: The pad marker shall be invalidated when the flight lands, so that no later power-up recovers against it. ← FLT-BROWN-02
- **FLT-BROWN-05**: /api/status shall say why a boot was cold: not a power event, no marker, on USB, at ground level, or no sample in time. ← FLT-BROWN-02
- **FLT-BROWN-06**: A recovered flight shall read pyro continuity before it rejoins, and shall produce altitude against the marker's ground from then on. ← FLT-BROWN-02
- **FLT-LOG-05**: The flight log shall not write flash until its RAM buffer has filled once, so that the launch shock window passes without a write in progress. ← FLT-BROWN-01
- **FLT-LOG-06**: The flight log shall be committed to the filesystem at least once per second while it is written, so that a flight which never lands keeps its record. Every write and commit shall run in the flash window between core1 work units. ← SYS-DATA-01
- **LUA-IO-01**: The web UI shall export the Lua program to a local file and import one back, so a program survives the loss of the filesystem that holds it -- a failed mount formats it, and a flash-geometry change moves it. (An OTA update does not: verified to leave every file in place.) ← SYS-CFG-01
- **LUA-IO-02**: An imported program shall land in the editor and not on the device, so a mis-picked file costs nothing until it is saved. ← LUA-IO-01
- **PIN-LABEL-01**: Every assignable pin shall carry the connector designator silkscreened on the board, and the web UI shall show it beside the GPIO number. ← SYS-CFG-01
- **PIN-BUZZ-01**: The buzzer shall be assignable to any pad the board declares capable of driving one, defaulting to the board's own buzzer pad where it fits one. ← SYS-CFG-01
- **PIN-BUZZ-02**: A pad driving the buzzer shall be reserved against Lua, and a pad holding a Lua role shall not be assignable as the buzzer. ← SYS-CFG-01
- **FLT-MACH-01**: The system shall not declare apogee while ascending faster than 100 ft/s, nor until it has been slower than that for 1 second. A flight that never exceeds 100 ft/s shall not be gated. ← FLT-PHASE-02
- **FLT-DESC-01**: The system shall determine the descent phase from the measured descent rate holding steady, not from which channel has been commanded. ← FLT-PHASE-02
- **FLT-DESC-02**: The system shall detect landing in every descent phase, so that a flight which deployed nothing still closes its flight log. ← FLT-PHASE-02

#### Altitude Clamping
- **PYR-ALT-01**: The system shall clamp altitude-based pyro settings to the barometric sensor ceiling. ← PYR-MODE-02, PYR-MODE-03, PYR-MODE-04
- **PYR-ALT-02**: The system shall emit a warning beep code when any altitude-based pyro setting exceeds the sensor ceiling. ← PYR-ALT-01

### Beep Codes
- **BUZ-CODE-01**: The beep vocabulary shall be the set of actions available at the pad: OK to fly, check pyro 1, check pyro 2, system failure. ← SYS-STATUS-02
- **BUZ-CODE-02**: Any condition that cannot be corrected at the rocket shall announce system failure, outranking a pyro fault. ← BUZ-CODE-01
- **BUZ-CODE-03**: The diagnosis shall be reported by name on /api/status, not encoded in the announcement. ← BUZ-CODE-01
- **BUZ-CODE-04**: Each outcome shall carry a stable key, a human-readable meaning, and a configurable sound. ← BUZ-CODE-01
- **BUZ-CODE-05**: A sound shall be a chirp, a steady tone, a beep count, or silence. ← BUZ-CODE-04
- **BUZ-CODE-06**: A beep count shall be 1 to 9 per group; a zero cannot be heard and a long count cannot be counted. ← BUZ-CODE-05
- **BUZ-CODE-07**: No two audible outcomes within a personality shall sound alike. ← BUZ-CODE-04
- **BUZ-CODE-08**: A personality that is wholly silent shall be refused. ← BUZ-CODE-04
- **BUZ-CODE-09**: The system shall hold three named personalities, one active. ← BUZ-CODE-04
- **BUZ-CODE-10**: A beep table that fails validation shall be rejected whole and the shipped personalities used. ← BUZ-CODE-04
- **BUZ-CODE-11**: The outcomes, their meanings and the personalities shall be served to the web interface so the firmware is the only place the vocabulary is written down. ← BUZ-CODE-04
- **BUZ-CODE-12**: A board with no beep.ini shall write the shipped personalities out. ← BUZ-CODE-04
- **BUZ-CODE-13**: The shipped defaults shall follow the Eggtimer Rocketry convention: a rapid chirp for OK to fly, 5 beeps for a drogue-channel fault, 4 for a main-channel fault, 2 for a hardware fault. ← BUZ-CODE-01
- **BUZ-CODE-14**: A valid beep table that cannot be stored shall be reported as a storage failure, not as a validation failure. ← BUZ-CODE-10

### Power-up Self-Test
- **FLT-BOOT-11**: The system shall test the pressure sensor before the pyro channels. ← FLT-PHASE-01
- **FLT-BOOT-12**: The system shall enter a terminal FAULT state, and announce system failure, when no pressure sensor answers. ← FLT-BOOT-11
- **FLT-BOOT-13**: The system shall enter FAULT when calibration produces no samples within 10 seconds, rather than proceeding to PAD_IDLE. ← FLT-BOOT-11
- **FLT-BOOT-14**: The system shall enter FAULT, and announce system failure, when the filesystem does not mount. ← FLT-BOOT-11
- **FLT-BOOT-15**: The system shall report every pad fault found, not only the first. ← SYS-STATUS-02
- **FLT-BOOT-16**: The system shall not report a continuity fault for a pyro channel released to Lua or configured as disabled. ← FLT-BOOT-15

#### Sampling Rates (v2.0)
- **FLT-RATE-01**: The system shall sample pressure at 50Hz (20ms) during PAD_IDLE, ASCENT, and DESCENT. ← FLT-PHASE-01, DD-001
- **FLT-RATE-02**: The system shall deliver pressure samples to the flight software in batches of 5 (100ms). ← FLT-RATE-01, PWR-SAMPLE-02
- **FLT-RATE-03**: The system shall reduce sampling to 1Hz during LANDED for power conservation. ← FLT-PHASE-03, SYS-PWR-01
- **FLT-RATE-04**: The sampling rate shall be a HAL responsibility; flight software processes whatever buffer it receives. ← HAL-02

---

## 2. Pre-Flight Status

### L1 User Need
- **UN-2**: The user needs to verify the system is ready before placing the rocket on the pad.

### L2 System Requirements
- **SYS-STATUS-01**: The system shall indicate readiness and faults audibly without requiring a display. ← UN-2
- **SYS-STATUS-02**: The system shall verify pyrotechnic circuit integrity before flight. ← UN-2

### L3 Subsystem Requirements
- **PYR-CONT-01**: The system shall check pyro continuity at least once per second during PAD_IDLE. ← SYS-STATUS-02
- **PYR-CONT-03**: The pad diagnosis and announcement shall be re-derived at every continuity check, so that a fault which appears or clears on the pad changes the announcement without a power cycle. On USB the verdict is re-derived but not said (USB-02). ← PYR-CONT-01, FLT-BOOT-15
- **PYR-CONT-02**: The system shall report continuity status (good, open, short) for each channel. ← SYS-STATUS-02
- **BUZ-STATUS-01**: The system shall emit distinct beep codes for each fault condition. ← SYS-STATUS-01

### L4 Implementation Requirements
- **BUZ-01**: The system shall announce one of four outcomes: OK to fly, check pyro 1, check pyro 2, system failure. ← BUZ-STATUS-01
- **BUZ-02**: The announcement shall repeat on a configurable cadence, defaulting to every 5 s until launch, so that silence means a fault rather than a finished message. Not while a USB host is attached (USB-02). ← BUZ-STATUS-01
- **FLT-BOOT-01**: The system shall complete a non-blocking boot sequence before entering PAD_IDLE. ← SYS-STATUS-01
- **FLT-BOOT-04**: The system shall wait at least 500ms after power-on before sensor communication. ← FLT-BOOT-01
- **FLT-BOOT-05**: The system shall detect and initialize the pressure sensor during boot. ← FLT-BOOT-01
- **FLT-BOOT-06**: The system shall initialize the pyrotechnic subsystem during boot. ← FLT-BOOT-01
- **FLT-BOOT-07**: The system shall perform an initial continuity check during boot. ← SYS-STATUS-02
- **FLT-BOOT-08**: The system shall calibrate ground pressure from the median of at least 10 readings, so one bad reading cannot bias it. ← FLT-BOOT-01
- **FLT-BOOT-09**: The system shall wait at least 2 seconds for sensor stabilization before calibration. ← FLT-BOOT-08

---

## 3. Flight Data Recovery

### L1 User Need
- **UN-3**: The user needs to retrieve flight performance data after recovery.

### L2 System Requirements
- **SYS-DATA-01**: The system shall record flight data throughout the flight. ← UN-3
- **SYS-DATA-02**: The system shall export flight data in a standard format. ← UN-3
- **SYS-DATA-03**: The system shall announce maximum altitude audibly after landing. ← UN-3

### L3 Subsystem Requirements
- **DAT-01**: The system shall store flight samples in a ring buffer of at least 4096 entries. ← SYS-DATA-01
- **DAT-02**: Each sample shall include: time, pressure, altitude, state, thrust flag, event. ← SYS-DATA-01
- **DAT-03**: Events shall be tagged on existing data samples, not stored as separate records. ← SYS-DATA-01
- **DAT-04**: The system shall log events: LAUNCH, ARMED, APOGEE, PYRO1_FIRE, PYRO2_FIRE, LANDING, and when they occur PYRO1/2_REFUSED, PYRO1/2_NOPEN, PYRO1/2_FAULT and MAIN_FORCED. ← SYS-DATA-01
- **DAT-06**: The system shall export flight data as CSV to persistent storage after landing. ← SYS-DATA-02
- **DAT-07**: The CSV shall include a metadata header with configuration and flight summary. ← SYS-DATA-02
- **BUZ-03**: The system shall play an altitude beep-out sequence after landing, holding it while a USB host is attached and resuming it when the host goes (USB-02, USB-04). ← SYS-DATA-03
- **BUZ-04**: The altitude beep-out shall encode each digit of the max altitude in configured units. ← BUZ-03
- **BUZ-05**: The digit 0 shall be encoded as 10 beeps. ← BUZ-04
- **BUZ-06**: The altitude beep-out shall repeat indefinitely. ← BUZ-03
- **BUZ-07**: The buzzer shall stop upon launch detection. ← FLT-PHASE-01

---

## 4. Configuration

### L1 User Need
- **UN-4**: The user needs to configure the system for different rockets and flight profiles.

### L2 System Requirements
- **SYS-CFG-01**: The system shall store configuration persistently across power cycles. ← UN-4
- **SYS-CFG-02**: The system shall allow configuration changes without special tools. ← UN-4
- **SYS-CFG-03**: The system shall validate configuration against sensor limitations. ← UN-4

### L3 Subsystem Requirements
- **CFG-01**: The system shall store configuration in an INI-format file on persistent storage. ← SYS-CFG-01
- **CFG-02**: The system shall parse fields: id, name, pyro1_mode, pyro1_value, pyro2_mode, pyro2_value, units. ← CFG-01
- **CFG-03**: The system shall support unit settings: cm, m, ft. ← CFG-02
- **CFG-04**: The system shall support pyro mode settings: none (disabled), delay, agl, fallen, speed. A mode the system cannot name shall be stored as none. ← CFG-02
- **CFG-05**: The system shall create a default configuration if the config file is missing. ← SYS-CFG-01
- **WEB-UI-02**: The web interface shall provide a guided configuration editor with input validation. Changing units shall convert the configured altitudes and speeds rather than reinterpret them, and a field with a length limit shall state it. ← SYS-CFG-02, SYS-CFG-03
- **WEB-UI-03**: The web interface shall warn when configuration has been saved but not applied. ← SYS-CFG-02

### L4 Implementation Requirements
- **CFG-06**: The system shall preserve existing config fields not present in a partial config file. ← CFG-02
- **CFG-07**: The system shall truncate id and name fields to 8 characters. ← CFG-02
- **CFG-08**: The system shall ignore unknown keys in the config file. ← CFG-02
- **CFG-09**: The system shall handle both CR+LF and LF line endings. ← CFG-01
- **FLT-BOOT-02**: The system shall read configuration from persistent storage during boot. ← SYS-CFG-01
- **FLT-BOOT-03**: The system shall create a default configuration file if none exists. ← CFG-05

---

## 5. Altitude Measurement

### L1 User Need
- **UN-5**: The user needs accurate altitude measurement for pyro deployment and data recording.

### L2 System Requirements
- **SYS-ALT-01**: The system shall measure altitude using barometric pressure. ← UN-5
- **SYS-ALT-02**: The system shall operate with multiple pressure sensor types. ← UN-5

### L3 Subsystem Requirements
- **SNS-PRES-01**: The system shall auto-detect the installed pressure sensor type. ← SYS-ALT-02
- **SNS-PRES-02**: The system shall apply a low-pass filter to pressure readings. ← SYS-ALT-01
- **SNS-ALT-01**: The system shall compute altitude from the difference between ground pressure and current pressure. ← SYS-ALT-01

### L4 Implementation Requirements
- **SNS-PRES-03**: The pressure filter shall initialize to the first raw reading without smoothing. ← SNS-PRES-02
- **SNS-PRES-04**: The pressure filter shall advance by at least 1 Pa per sample when the raw value differs from the filtered value. ← SNS-PRES-02
- **SNS-PRES-05**: A sensor conversion shall be read no sooner than its worst-case conversion time after the command that started it. ← SNS-PRES-01
- **SNS-PRES-06**: A reading the sensor cannot produce -- a zero conversion, or a pressure outside its rated range -- shall be discarded and counted, not filtered. ← SNS-PRES-02
- **SNS-PRES-07**: A single-sample outlier shall not reach the pressure filter. The median of the newest three readings stands between the range check and the filter, carrying the middle reading's time. ← SNS-PRES-02
- **SNS-ALT-02**: The system shall clamp computed altitude to a maximum of 8000 meters. ← SNS-ALT-01
- **SNS-ALT-03**: The system shall clamp computed altitude to a minimum of 0 meters. ← SNS-ALT-01
- **SNS-ALT-04**: Vertical speed shall be taken from altitude that is not clamped; SNS-ALT-02 and SNS-ALT-03 clamp only the altitude that is reported. ← SNS-ALT-01

---

## 6. Telemetry

### L1 User Need
- **UN-6**: The user needs real-time flight data transmitted for ground monitoring.

### L2 System Requirements
- **SYS-TEL-01**: The system shall transmit flight data via serial interface during flight. ← UN-6

### L3 Subsystem Requirements
- **TEL-01**: The system shall output telemetry in $PYRO NMEA sentence format. ← SYS-TEL-01
- **TEL-02**: Each telemetry sentence shall include an XOR checksum. ← SYS-TEL-01
- **TEL-03**: The system shall output telemetry at 10Hz during ASCENT and DESCENT. ← SYS-TEL-01
- **TEL-04**: The system shall output telemetry at 1Hz during PAD_IDLE and LANDED. ← SYS-TEL-01
- **TEL-05**: The system shall not output telemetry during boot states or in FAULT. A faulted board shall instead send a `!FAULT` diagnostic line every 5 s. ← SYS-TEL-01

### L4 Implementation Requirements
- **TEL-06**: Each sentence shall include: sequence, state, altitude, speed, max altitude, pressure, flight time, flags. ← TEL-01
- **TEL-07**: The state field shall map: PAD_IDLE=0, ASCENT=1, FALLING=2, DROGUE_DESCENT=3, CHUTE_DESCENT=4, LANDED=5. ← TEL-06
- **TEL-08**: The flags field shall encode: P1 continuity, P2 continuity, P1 fired, P2 fired, armed, apogee. ← TEL-06
- **TEL-09**: The sequence number shall increment with each sentence. ← TEL-01
- **TEL-10**: The thrust flag shall only be set during ASCENT when under thrust. ← TEL-06

---

## 7. Pyro Fault Protection

### L1 User Need
- **UN-7**: The user needs protection against pyrotechnic faults that could damage the system or cause unsafe conditions.

### L2 System Requirements
- **SYS-FAULT-01**: The system shall limit pyro drive current to prevent damage. ← UN-7
- **SYS-FAULT-02**: The system shall detect pyro fault conditions. ← UN-7
- **SYS-FAULT-03**: The system shall notify the user of pyro fault conditions. ← UN-7

### L3 Subsystem Requirements
- **PYR-FAULT-01**: The system shall disable pyro drive when current exceeds 1.5A. ← SYS-FAULT-01
- **PYR-FAULT-02**: The system shall detect when pyro drive has exceeded the current limit. ← SYS-FAULT-02
- **PYR-FAULT-03**: The system shall indicate to the user that an overcurrent condition occurred during pyro firing. ← SYS-FAULT-03
- **PYR-VERIFY-01**: The system shall verify pyro circuit opened after firing by reading continuity. ← SYS-FAULT-02

---

## 8. Web Interface & Network

### L1 User Need
- **UN-8**: The user needs to monitor, configure, and update the system from a computer without special software.

### L2 System Requirements
- **SYS-WEB-01**: The system shall provide a web interface accessible via USB connection. ← UN-8
- **SYS-WEB-02**: The system shall be discoverable on the network without manual IP configuration. ← UN-8

### L3 Subsystem Requirements
- **WEB-NET-01**: The system shall present a USB network interface to the host computer. ← SYS-WEB-01
- **WEB-NET-02**: The system shall serve DHCP, assigning itself 192.168.7.1. ← SYS-WEB-01
- **WEB-NET-03**: The system shall advertise its hostname via mDNS. ← SYS-WEB-02
- **WEB-NET-04**: The system shall advertise a DNS-SD service for automatic discovery. ← SYS-WEB-02
- **WEB-API-01**: The system shall serve device status as JSON at `/api/status`. ← SYS-WEB-01
- **WEB-API-02**: The system shall serve the configuration file at `/api/config` (GET). ← SYS-WEB-01
- **WEB-API-03**: The system shall accept configuration updates at `/api/config` (POST) and write to persistent storage. ← SYS-WEB-01
- **WEB-API-04**: The system shall accept firmware updates at `/api/ota` (POST), answer before it restarts, and answer `Expect: 100-continue`. ← SYS-WEB-01
- **WEB-API-05**: The system shall trigger a device restart at `/api/reboot` (POST). ← SYS-WEB-01
- **WEB-API-06**: The system shall serve flight data as CSV at `/api/flight.csv`. ← SYS-WEB-01
- **WEB-API-07**: All API responses shall include CORS headers. ← SYS-WEB-01
- **WEB-API-08**: The system shall refuse every POST, and the bench capture, while the rocket is in flight (ASCENT through CHUTE_DESCENT). ← PYR-SAFE-04, SYS-WEB-01
- **WEB-API-09**: The system shall erase the flight log on request at `/api/flight/erase` (POST), unless the log is being written. ← DAT-06
- **WEB-HTTP-01**: The HTTP server shall treat each connection as a byte stream: a request shall be answered the same however TCP divides it into segments, including a header block or body split at any byte and more than one request in a single segment. ← SYS-WEB-01
- **WEB-HTTP-02**: Every response shall be framed by Content-Length and carry Connection: close; one request is served per connection. ← SYS-WEB-01
- **WEB-HTTP-03**: The server shall read a request body only as fast as it consumes it, so that TCP flow control, not a refused segment, holds back a sender while the body waits for the flash window. ← SYS-WEB-01, DD-035
- **WEB-HTTP-04**: The server shall refuse a malformed or oversized request with its HTTP status: 400 malformed, 405 unsupported method (with Allow), 411 no length, 413 body too large, 414 path too long, 431 header block too large. ← SYS-WEB-01
- **WEB-HTTP-05**: The server shall do its HTTP work from the main loop, never inside a network stack callback, and shall not depend on the stack beyond moving bytes, so that the stack can be replaced. ← SYS-WEB-01
- **WEB-UI-01**: The web interface shall display device status in the configured units. ← SYS-WEB-01
- **WEB-UI-04**: The web interface shall display flight summary data and allow CSV download. The summary shall come from the flight log alone, be re-read whenever it is shown, and name the flight it describes; flight time shall stop at the landing. ← SYS-WEB-01
- **WEB-UI-05**: The web interface shall support firmware upload and update checking. ← SYS-WEB-01

---

## 9. Firmware Update

### L1 User Need
- **UN-9**: The user needs to update firmware safely without risk of bricking the device.

### L2 System Requirements
- **SYS-OTA-01**: The system shall support firmware updates without physical access to the board. ← UN-9
- **SYS-OTA-02**: The system shall recover from a failed firmware update. ← UN-9

### L3 Subsystem Requirements
- **OTA-01**: The system shall support over-the-air firmware updates via HTTP. ← SYS-OTA-01
- **OTA-02**: The system shall write new firmware to an inactive slot while continuing to run. ← SYS-OTA-01
- **OTA-03**: The system shall automatically revert to the previous firmware if the new firmware does not confirm within one boot cycle. ← SYS-OTA-02
- **OTA-04**: A failed or interrupted update shall not affect the currently running firmware. ← SYS-OTA-02

---

## 10. Portability & Testability

### L1 User Need
- **UN-10**: Contributors need to develop and test flight software without flight hardware.

### L2 System Requirements
- **SYS-PORT-01**: The flight software shall be testable on a host computer without hardware. ← UN-10
- **SYS-PORT-02**: The flight software shall be runnable in a browser-based simulation. ← UN-10

### L3 Subsystem Requirements
- **HAL-01**: Flight logic source files shall contain no platform-specific code or conditional compilation. ← SYS-PORT-01
- **HAL-02**: All hardware interaction shall occur through a defined HAL interface. ← SYS-PORT-01
- **HAL-03**: The HAL interface shall support at least three implementations: hardware, test, simulation. ← SYS-PORT-01, SYS-PORT-02
- **HAL-04**: The same flight logic source files shall compile unchanged for all targets. ← HAL-01

---

## 11. Build System

- **BLD-01**: The build system shall produce firmware for the RP2040 target. ← SYS-PORT-01
- **BLD-02**: The build system shall produce host-compiled test executables. ← SYS-PORT-01
- **BLD-03**: The build system shall produce a host-compiled flight simulator. ← SYS-PORT-02
- **BLD-04**: The build system shall auto-generate a version header from the VERSION file.
- **BLD-05**: The build system shall support A/B OTA firmware images. ← OTA-02

---

## 12. Test System

- **TST-01**: Unit tests shall verify individual functions in isolation with mock hardware. ← SYS-PORT-01
- **TST-02**: Integration tests shall verify complete flight sequences using recorded trajectory data. ← SYS-PORT-01
- **TST-03**: Closed-loop tests shall verify flight behavior with physics simulation feedback. ← SYS-PORT-01
- **TST-04**: Closed-loop tests shall cover all four pyro firing modes. ← PYR-MODE-01..04
- **TST-05**: Closed-loop tests shall cover flight altitudes from 100ft to 100km. ← SYS-DEPLOY-01
- **TST-06**: Closed-loop tests shall verify that chute deployment reduces descent rate. ← SYS-DEPLOY-01
- **TST-07**: Web UI tests shall verify status display, configuration editing, and firmware update flows. ← SYS-WEB-01
- **TST-08**: All tests shall run in CI on every push to main.
- **TST-09**: Tests shall be traceable to requirements.

---

## 13. Power Management (v2.0) ✅ Done

### L1 User Need
- **UN-11**: The user needs the flight computer to operate on battery for extended pad time.

### L2 System Requirements
- **SYS-PWR-01**: The system shall minimize CPU active time to conserve battery. ← UN-11
- **SYS-PWR-02**: The system shall perform all I/O autonomously without CPU involvement. ← SYS-PWR-01

### L3 Subsystem Requirements
- **PWR-SAMPLE-01**: Pressure sampling shall run autonomously at 50Hz via ISR, DMA, or second core. ← SYS-PWR-02 ✅ async_task.h + hal_pressure_fifo_*
- **PWR-SAMPLE-02**: Pressure data shall be delivered to the flight software in batches of 5 samples (100ms). ← PWR-SAMPLE-01 ✅ hal_pressure_batch_t + flight_process_samples()
- **PWR-TELEM-01**: Telemetry transmission shall be asynchronous via ISR or DMA. ← SYS-PWR-02 ✅ v2-10 UART0 TX ring buffer + ISR
- **PWR-BUZZ-01**: Buzzer patterns shall be played autonomously via async task runner. ← SYS-PWR-02 ✅ v2-7 buzzer async state machine
- **PWR-USB-01**: USB servicing shall run autonomously via timer ISR or second core. ← SYS-PWR-02 (deferred to v2.1)
- **PWR-SLEEP-01**: The CPU shall sleep between pressure buffer delivery events. ← SYS-PWR-01 ✅ hal_sleep_until_event() / __wfe()
- **PWR-LOG-01**: Data logging shall buffer in RAM and flush to flash asynchronously. ← SYS-PWR-02 ✅ v2-9 hal_log_sample() 512-byte ring, 200ms flush task

### L4 Implementation Requirements
- **PWR-LOG-02**: `hal_log_start()` shall open the flight log file and register a flush task. ← PWR-LOG-01
- **PWR-LOG-03**: `hal_log_sample()` shall be non-blocking: it copies one formatted line into a RAM buffer. ← PWR-LOG-01
- **PWR-LOG-04**: `hal_log_stop()` shall signal the flush task to finalize and close the log file. ← PWR-LOG-01
- **PWR-BUZZ-02**: The buzzer pattern player shall use three states: IDLE → ENCODE → PLAYING. ← PWR-BUZZ-01
- **PWR-BUZZ-03**: Pattern steps shall be computed at request time from the beep code or altitude value. ← PWR-BUZZ-01
- **PWR-TELEM-02**: `hal_telemetry_send()` shall complete in O(n) time with no UART stall. ← PWR-TELEM-01
- **PWR-TELEM-03**: The UART TX ring shall be at least 512 bytes; overflow shall drop the end of the sentence. ← PWR-TELEM-01

## 14. Telemetry Formatting (v2.0) ✅ Done

### L2 System Requirements
- **SYS-TELEM-FMT-01**: The telemetry format shall be configurable without changing flight software. ← UN-4

### L3 Subsystem Requirements
- **TELEM-FMT-01**: A telemetry formatter module shall convert flight events to protocol-specific messages. ← SYS-TELEM-FMT-01 ✅ telemetry_formatter.c
- **TELEM-FMT-02**: The formatter shall support event messages (apogee, pyro fire, landing) and periodic state messages. ← TELEM-FMT-01 ✅
- **TELEM-FMT-03**: The HAL telemetry transport shall be a raw byte interface with no protocol knowledge. ← TELEM-FMT-01 ✅ hal_telemetry_send(const char*)

## 15. Configuration System (v2.0) ✅ Done

### L2 System Requirements
- **SYS-CFG-04**: Adding a configuration field shall require changes to a single location. ← UN-4

### L3 Subsystem Requirements
- **CFG-TABLE-01**: All configuration fields shall be defined in a single table that generates the struct, parser, serializer, and defaults. ← SYS-CFG-04 ✅ config_fields.h X-macro
- **CFG-TABLE-02**: A round-trip test shall automatically verify every field survives serialize → parse. ← CFG-TABLE-01 ✅ test_config.c (15 tests)
- **CFG-SUBSYS-01**: Each subsystem (telemetry, logging, buzzer) shall have configurable parameters: `telem_format` and `telem_rate_hz`, `log_rate_hz`, and the beep personalities in `beep.ini`. Every configuration key shall be read by something. ← UN-4 ✅

## 16. Ground Test (v2.0) ✅ Done

### L1 User Need
- **UN-12**: The user needs to verify pyro circuits and system behavior on the ground without a computer.

### L2 System Requirements
- **SYS-TEST-01**: The system shall support ground test operations via serial commands. ← UN-12, DD-011

### L3 Subsystem Requirements
- **GND-TEST-01**: The system shall accept serial commands to replay status and altitude beep codes. ← SYS-TEST-01 ✅
- **GND-TEST-02**: The system shall accept serial commands to arm and fire individual pyro channels for ground testing. ← SYS-TEST-01 ✅
- **GND-TEST-03**: Ground test arm shall require a multi-step confirmation and auto-disarm after 3 seconds. ← SYS-TEST-01 ✅
- **GND-TEST-04**: Ground test shall be available only during PAD_IDLE state. ← PYR-SAFE-04 ✅

## 17. On USB

### L1 User Need
- **UN-13**: The user needs a board on the bench, plugged into a computer or a charger, to stay quiet and never behave as though it were flying.

### L2 System Requirements
- **SYS-USB-01**: While attached to USB, the system shall not detect a flight and shall not announce its status, unless the operator has put it in test mode. ← UN-13

### L3 Subsystem Requirements
- **USB-01**: While a USB host is attached and test mode is off, the system shall not declare launch, shall not rejoin a flight after a power event, and shall not write the pad marker. ← SYS-USB-01
- **USB-02**: While a USB host is attached and test mode is off, the system shall not announce the pad verdict, a system failure, or the altitude beep-out. The verdict and diagnosis stay on /api/status. This overrides BUZ-02, BUZ-03, FLT-BOOT-12 and FLT-BOOT-14 while attached. ← SYS-USB-01
- **USB-03**: On attach, and on leaving test mode while attached, the system shall play one OK-on-USB double chirp, and nothing more of its own until detached. Sounds an operator asks for (the web beep audition, the ground-test BEEP commands) still play. ← SYS-USB-01
- **USB-04**: On detach, the system shall resume what it would have been saying, and restart the pad marker's 10 s dwell. ← SYS-USB-01
- **USB-05**: From launch to landing, the attach state shall be ignored. ← SYS-USB-01
- **USB-06**: Attachment to a charger shall count as USB attachment. ← SYS-USB-01 ❌ Not possible on MK1A/MK1B/MK1C: VBUS reaches only the charger IC. See DD-037.
- **USB-07**: The system shall judge a host attached only on evidence that a host is present, so that every detection error leaves launch detection on. ← SYS-USB-01, SYS-DEPLOY-01
- **USB-08**: An operator-selected test mode shall make the system behave on USB as it does on battery: launch detection, deployment, the pad marker and every announcement. It shall be held in RAM so that every boot starts with it off, shall not change from launch to landing, shall be set from the web interface after a confirmation, and shall be reported on /api/status. ← SYS-USB-01, UN-12

