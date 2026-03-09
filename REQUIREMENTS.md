# Pyro MK1B Requirements

## 1. Flight Operations

### 1.1 Boot Sequence
- **FLT-BOOT-01**: The system shall complete a non-blocking boot sequence before entering PAD_IDLE.
- **FLT-BOOT-02**: The system shall read configuration from persistent storage during boot.
- **FLT-BOOT-03**: The system shall create a default configuration file if none exists.
- **FLT-BOOT-04**: The system shall wait at least 500ms after power-on before attempting sensor communication.
- **FLT-BOOT-05**: The system shall detect and initialize the pressure sensor during boot.
- **FLT-BOOT-06**: The system shall initialize the pyrotechnic subsystem during boot.
- **FLT-BOOT-07**: The system shall perform an initial continuity check during boot.
- **FLT-BOOT-08**: The system shall calibrate ground pressure by averaging at least 10 readings.
- **FLT-BOOT-09**: The system shall wait at least 2 seconds for sensor stabilization before calibration.

### 1.2 Launch Detection
- **FLT-LAUNCH-01**: The system shall transition from PAD_IDLE to ASCENT when filtered altitude exceeds 10 meters.
- **FLT-LAUNCH-02**: The system shall remain in PAD_IDLE when altitude is at or below 10 meters.
- **FLT-LAUNCH-03**: The system shall record the launch time by backdating to the first sample above 50cm altitude.
- **FLT-LAUNCH-04**: The system shall log a LAUNCH event when transitioning to ASCENT.
- **FLT-LAUNCH-05**: The system shall stop the buzzer upon launch detection.

### 1.3 Ascent
- **FLT-ASC-01**: The system shall track and record the maximum altitude achieved during ascent.
- **FLT-ASC-02**: The system shall compute vertical speed from consecutive altitude samples.
- **FLT-ASC-03**: The system shall detect thrust phase when vertical speed is increasing.
- **FLT-ASC-04**: The system shall arm pyrotechnics when vertical speed drops below 10 m/s.
- **FLT-ASC-05**: The system shall log an ARMED event when pyrotechnics are armed.
- **FLT-ASC-06**: The system shall not arm pyrotechnics while vertical speed exceeds 10 m/s.

### 1.4 Apogee Detection
- **FLT-APO-01**: The system shall detect apogee when vertical speed crosses zero while pyros are armed.
- **FLT-APO-02**: The system shall transition from ASCENT to DESCENT upon apogee detection.
- **FLT-APO-03**: The system shall log an APOGEE event at the transition.
- **FLT-APO-04**: The system shall not detect apogee before pyros are armed.

### 1.5 Landing Detection
- **FLT-LAND-01**: The system shall detect landing when altitude change is less than 1 meter between consecutive samples for at least 1 second.
- **FLT-LAND-02**: The system shall require vertical speed below 2 m/s for landing detection.
- **FLT-LAND-03**: The system shall require altitude below 30 meters AGL for landing detection.
- **FLT-LAND-04**: The system shall transition from DESCENT to LANDED upon landing detection.
- **FLT-LAND-05**: The system shall log a LANDING event at the transition.
- **FLT-LAND-06**: The system shall remain in LANDED state permanently after landing.

### 1.6 Sampling Rates
- **FLT-RATE-01**: The system shall sample pressure at least every 10ms during PAD_IDLE.
- **FLT-RATE-02**: The system shall sample pressure at least every 100ms during ASCENT.
- **FLT-RATE-03**: The system shall sample pressure at least every 50ms during DESCENT.
- **FLT-RATE-04**: The system shall sample pressure at least every 1000ms during LANDED.

## 2. Pyrotechnic Control

### 2.1 Firing Modes
- **PYR-MODE-01**: The system shall support a DELAY firing mode that fires N seconds after apogee.
- **PYR-MODE-02**: The system shall support an AGL firing mode that fires when altitude drops below a threshold.
- **PYR-MODE-03**: The system shall support a FALLEN firing mode that fires when altitude drops a specified distance from maximum.
- **PYR-MODE-04**: The system shall support a SPEED firing mode that fires when descent speed exceeds a threshold.
- **PYR-MODE-05**: The system shall not fire any pyro before apogee is detected.

### 2.2 Firing Safety
- **PYR-SAFE-01**: The system shall not fire a pyro channel that has no continuity.
- **PYR-SAFE-02**: The system shall not fire a pyro channel while another channel is actively firing.
- **PYR-SAFE-03**: The system shall fire each channel at most once per flight (except re-fire, see PYR-REFIRE-01).
- **PYR-SAFE-04**: The system shall log a PYRO fire event for each channel fired.

### 2.3 Re-fire
- **PYR-REFIRE-01**: The system shall attempt to re-fire a pyro channel if descent speed exceeds 30 m/s between 1 and 1.5 seconds after the initial fire and continuity is still present.

### 2.4 Continuity
- **PYR-CONT-01**: The system shall check pyro continuity at least once per second during PAD_IDLE.
- **PYR-CONT-02**: The system shall report continuity status (good, open, short) for each channel.

### 2.5 Altitude Limits
- **PYR-ALT-01**: The system shall clamp altitude-based pyro settings (AGL, FALLEN, SPEED) to the barometric sensor ceiling.
- **PYR-ALT-02**: The system shall emit a warning beep code when any altitude-based pyro setting exceeds the sensor ceiling.

## 3. Sensor

### 3.1 Pressure
- **SNS-PRES-01**: The system shall auto-detect the installed pressure sensor type.
- **SNS-PRES-02**: The system shall apply a low-pass filter to pressure readings.
- **SNS-PRES-03**: The pressure filter shall initialize to the first raw reading without smoothing.
- **SNS-PRES-04**: The pressure filter shall advance by at least 1 Pa per sample when the raw value differs from the filtered value.

### 3.2 Altitude
- **SNS-ALT-01**: The system shall compute altitude from the difference between ground pressure and current pressure.
- **SNS-ALT-02**: The system shall clamp computed altitude to a maximum of 8000 meters.
- **SNS-ALT-03**: The system shall clamp computed altitude to a minimum of 0 meters.

## 4. Configuration

- **CFG-01**: The system shall store configuration in an INI-format file on persistent storage.
- **CFG-02**: The system shall parse configuration fields: id, name, pyro1_mode, pyro1_value, pyro2_mode, pyro2_value, units.
- **CFG-03**: The system shall support unit settings: cm, m, ft.
- **CFG-04**: The system shall support pyro mode settings: delay, agl, fallen, speed.
- **CFG-05**: The system shall create a default configuration if the config file is missing.
- **CFG-06**: The system shall preserve existing config fields not present in a partial config file.
- **CFG-07**: The system shall truncate id and name fields to 8 characters.
- **CFG-08**: The system shall ignore unknown keys in the config file.
- **CFG-09**: The system shall handle both CR+LF and LF line endings.

## 5. Telemetry

- **TEL-01**: The system shall output telemetry in $PYRO NMEA sentence format.
- **TEL-02**: Each telemetry sentence shall include an XOR checksum.
- **TEL-03**: The system shall output telemetry at 10Hz during ASCENT and DESCENT.
- **TEL-04**: The system shall output telemetry at 1Hz during PAD_IDLE and LANDED.
- **TEL-05**: The system shall not output telemetry during boot states.
- **TEL-06**: Each telemetry sentence shall include: sequence number, state, altitude, speed, max altitude, pressure, flight time, flags.
- **TEL-07**: The telemetry state field shall map: PAD_IDLE=0, ASCENT=1, DESCENT=2, LANDED=3.
- **TEL-08**: The telemetry flags field shall encode: P1 continuity, P2 continuity, P1 fired, P2 fired, armed, apogee.
- **TEL-09**: The telemetry sequence number shall increment with each sentence.
- **TEL-10**: The thrust flag shall only be set during ASCENT when under thrust.

## 6. Buzzer

- **BUZ-01**: The system shall play a startup sequence of 10 chirps followed by a status code.
- **BUZ-02**: The system shall play the status code twice then stop.
- **BUZ-03**: The system shall play an altitude beep-out sequence after landing.
- **BUZ-04**: The altitude beep-out shall encode each digit of the max altitude in configured units.
- **BUZ-05**: The digit 0 shall be encoded as 10 beeps.
- **BUZ-06**: The altitude beep-out shall repeat indefinitely.
- **BUZ-07**: The buzzer shall stop upon launch detection.

## 7. Data Logging

- **DAT-01**: The system shall store flight samples in a ring buffer of at least 4096 entries.
- **DAT-02**: Each sample shall include: time, pressure, altitude, state, thrust flag, event.
- **DAT-03**: Events shall be tagged on existing data samples, not stored as separate records.
- **DAT-04**: The system shall log events: LAUNCH, ARMED, APOGEE, PYRO1_FIRE, PYRO2_FIRE, LANDING.
- **DAT-05**: The ring buffer shall protect apogee-region samples from overwrite.
- **DAT-06**: The system shall export flight data as CSV to persistent storage after landing.
- **DAT-07**: The CSV shall include a metadata header with configuration and flight summary.

## 8. Hardware Abstraction

- **HAL-01**: Flight logic source files shall contain no platform-specific code or conditional compilation.
- **HAL-02**: All hardware interaction shall occur through a defined HAL interface.
- **HAL-03**: The HAL interface shall support at least three implementations: hardware, test, simulation.
- **HAL-04**: The same flight logic source files shall compile unchanged for all targets.

## 9. Build System

- **BLD-01**: The build system shall produce firmware for the RP2040 target.
- **BLD-02**: The build system shall produce host-compiled test executables.
- **BLD-03**: The build system shall produce a host-compiled flight simulator.
- **BLD-04**: The build system shall auto-generate a version header from the VERSION file.
- **BLD-05**: The build system shall support A/B OTA firmware images.

## 10. Test System

- **TST-01**: Unit tests shall verify individual functions in isolation with mock hardware.
- **TST-02**: Integration tests shall verify complete flight sequences using recorded trajectory data.
- **TST-03**: Closed-loop tests shall verify flight behavior with physics simulation feedback.
- **TST-04**: Closed-loop tests shall cover all four pyro firing modes.
- **TST-05**: Closed-loop tests shall cover flight altitudes from 100ft to 100km.
- **TST-06**: Closed-loop tests shall verify that chute deployment reduces descent rate.
- **TST-07**: Web UI tests shall verify status display, configuration editing, and firmware update flows.
- **TST-08**: All tests shall run in CI on every push to main.
- **TST-09**: Tests shall be traceable to requirements.
