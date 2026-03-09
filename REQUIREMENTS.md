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

#### Firing Safety
- **PYR-SAFE-01**: The system shall not fire a channel that has no continuity. ← SYS-DEPLOY-03
- **PYR-SAFE-02**: The system shall not fire two channels simultaneously. ← SYS-DEPLOY-02
- **PYR-SAFE-03**: The system shall fire each channel at most once per flight (except re-fire). ← SYS-DEPLOY-02
- **PYR-SAFE-04**: The system shall not fire any pyro before apogee is detected. ← SYS-DEPLOY-03

### L4 Implementation Requirements

#### Launch Detection
- **FLT-LAUNCH-01**: The system shall transition to ASCENT when filtered altitude exceeds 10 meters. ← FLT-PHASE-01
- **FLT-LAUNCH-02**: The system shall remain in PAD_IDLE when altitude is at or below 10 meters. ← FLT-PHASE-01
- **FLT-LAUNCH-03**: The system shall record launch time by backdating to the first sample above 50cm. ← FLT-PHASE-01
- **FLT-LAUNCH-04**: The system shall log a LAUNCH event at the transition. ← FLT-PHASE-01
- **FLT-LAUNCH-05**: The system shall stop the buzzer upon launch detection. ← FLT-PHASE-01

#### Apogee Detection
- **FLT-APO-01**: The system shall detect apogee when vertical speed crosses zero while pyros are armed. ← FLT-PHASE-02
- **FLT-APO-02**: The system shall transition from ASCENT to DESCENT upon apogee detection. ← FLT-PHASE-02
- **FLT-APO-03**: The system shall log an APOGEE event at the transition. ← FLT-PHASE-02
- **FLT-APO-04**: The system shall not detect apogee before pyros are armed. ← FLT-PHASE-02, PYR-SAFE-04

#### Pyro Arming
- **FLT-ASC-01**: The system shall track maximum altitude during ascent. ← FLT-PHASE-02
- **FLT-ASC-02**: The system shall compute vertical speed from consecutive altitude samples. ← FLT-PHASE-02
- **FLT-ASC-03**: The system shall detect thrust phase when vertical speed is increasing. ← FLT-PHASE-01
- **FLT-ASC-04**: The system shall arm pyrotechnics when vertical speed drops below 10 m/s. ← PYR-SAFE-04
- **FLT-ASC-05**: The system shall log an ARMED event when pyrotechnics are armed. ← PYR-SAFE-04
- **FLT-ASC-06**: The system shall not arm pyrotechnics while vertical speed exceeds 10 m/s. ← PYR-SAFE-04

#### Landing Detection
- **FLT-LAND-01**: The system shall detect landing when altitude change is less than 1 meter between consecutive samples for at least 1 second. ← FLT-PHASE-03
- **FLT-LAND-02**: The system shall require vertical speed below 2 m/s for landing detection. ← FLT-PHASE-03
- **FLT-LAND-03**: The system shall require altitude below 30 meters AGL for landing detection. ← FLT-PHASE-03
- **FLT-LAND-04**: The system shall transition from DESCENT to LANDED upon landing detection. ← FLT-PHASE-03
- **FLT-LAND-05**: The system shall log a LANDING event at the transition. ← FLT-PHASE-03
- **FLT-LAND-06**: The system shall remain in LANDED state permanently after landing. ← FLT-PHASE-03

#### Pyro Re-fire
- **PYR-REFIRE-01**: The system shall re-fire a channel if descent speed exceeds 30 m/s between 1 and 1.5 seconds after initial fire and continuity is still present. ← SYS-DEPLOY-01

#### Altitude Clamping
- **PYR-ALT-01**: The system shall clamp altitude-based pyro settings to the barometric sensor ceiling. ← PYR-MODE-02, PYR-MODE-03, PYR-MODE-04
- **PYR-ALT-02**: The system shall emit a warning beep code when any altitude-based pyro setting exceeds the sensor ceiling. ← PYR-ALT-01

#### Sampling Rates
- **FLT-RATE-01**: The system shall sample pressure at least every 10ms during PAD_IDLE. ← FLT-PHASE-01
- **FLT-RATE-02**: The system shall sample pressure at least every 100ms during ASCENT. ← FLT-PHASE-02
- **FLT-RATE-03**: The system shall sample pressure at least every 50ms during DESCENT. ← FLT-PHASE-03
- **FLT-RATE-04**: The system shall sample pressure at least every 1000ms during LANDED. ← FLT-PHASE-03

---

## 2. Pre-Flight Status

### L1 User Need
- **UN-2**: The user needs to verify the system is ready before placing the rocket on the pad.

### L2 System Requirements
- **SYS-STATUS-01**: The system shall indicate readiness and faults audibly without requiring a display. ← UN-2
- **SYS-STATUS-02**: The system shall verify pyrotechnic circuit integrity before flight. ← UN-2

### L3 Subsystem Requirements
- **PYR-CONT-01**: The system shall check pyro continuity at least once per second during PAD_IDLE. ← SYS-STATUS-02
- **PYR-CONT-02**: The system shall report continuity status (good, open, short) for each channel. ← SYS-STATUS-02
- **BUZ-STATUS-01**: The system shall emit distinct beep codes for each fault condition. ← SYS-STATUS-01

### L4 Implementation Requirements
- **BUZ-01**: The system shall play a startup sequence of 10 chirps followed by a status code. ← BUZ-STATUS-01
- **BUZ-02**: The system shall play the status code twice then stop. ← BUZ-STATUS-01
- **FLT-BOOT-01**: The system shall complete a non-blocking boot sequence before entering PAD_IDLE. ← SYS-STATUS-01
- **FLT-BOOT-04**: The system shall wait at least 500ms after power-on before sensor communication. ← FLT-BOOT-01
- **FLT-BOOT-05**: The system shall detect and initialize the pressure sensor during boot. ← FLT-BOOT-01
- **FLT-BOOT-06**: The system shall initialize the pyrotechnic subsystem during boot. ← FLT-BOOT-01
- **FLT-BOOT-07**: The system shall perform an initial continuity check during boot. ← SYS-STATUS-02
- **FLT-BOOT-08**: The system shall calibrate ground pressure by averaging at least 10 readings. ← FLT-BOOT-01
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
- **DAT-04**: The system shall log events: LAUNCH, ARMED, APOGEE, PYRO1_FIRE, PYRO2_FIRE, LANDING. ← SYS-DATA-01
- **DAT-06**: The system shall export flight data as CSV to persistent storage after landing. ← SYS-DATA-02
- **DAT-07**: The CSV shall include a metadata header with configuration and flight summary. ← SYS-DATA-02
- **BUZ-03**: The system shall play an altitude beep-out sequence after landing. ← SYS-DATA-03
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
- **CFG-04**: The system shall support pyro mode settings: delay, agl, fallen, speed. ← CFG-02
- **CFG-05**: The system shall create a default configuration if the config file is missing. ← SYS-CFG-01
- **WEB-UI-02**: The web interface shall provide a guided configuration editor with input validation. ← SYS-CFG-02, SYS-CFG-03
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
- **SNS-ALT-02**: The system shall clamp computed altitude to a maximum of 8000 meters. ← SNS-ALT-01
- **SNS-ALT-03**: The system shall clamp computed altitude to a minimum of 0 meters. ← SNS-ALT-01

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
- **TEL-05**: The system shall not output telemetry during boot states. ← SYS-TEL-01

### L4 Implementation Requirements
- **TEL-06**: Each sentence shall include: sequence, state, altitude, speed, max altitude, pressure, flight time, flags. ← TEL-01
- **TEL-07**: The state field shall map: PAD_IDLE=0, ASCENT=1, DESCENT=2, LANDED=3. ← TEL-06
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
- **WEB-API-04**: The system shall accept firmware updates at `/api/ota` (POST). ← SYS-WEB-01
- **WEB-API-05**: The system shall trigger a device restart at `/api/reboot` (POST). ← SYS-WEB-01
- **WEB-API-06**: The system shall serve flight data as CSV at `/api/flight.csv`. ← SYS-WEB-01
- **WEB-API-07**: All API responses shall include CORS headers. ← SYS-WEB-01
- **WEB-UI-01**: The web interface shall display device status in the configured units. ← SYS-WEB-01
- **WEB-UI-04**: The web interface shall display flight summary data and allow CSV download. ← SYS-WEB-01
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
