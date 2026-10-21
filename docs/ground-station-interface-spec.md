# Ground Station Interface Specification

## Altimeter Serial Telemetry Protocol

*April 2026 | Revision 1.2*

---

## 1. Purpose

This document specifies the serial telemetry interface between the altimeter board and the LoRa tracker board. It is the single reference for the $PYRO NMEA protocol: the state numbering scheme, the event sentences, and the flags bitfield. Any Claude session working on the altimeter firmware should read this document before making changes to the telemetry output.

The tracker firmware, the Data Source Manager (DSM), and the ground station web application all depend on this interface. Changes to the altimeter must preserve compatibility with these downstream consumers.

---

## 2. Background: The State Mismatch Problem

The ground station knows six state codes (Section 4). A code that names another phase shows the wrong flight phase, and a code it does not know shows no recognised state, which its health monitoring takes for signal loss while the tracker is still transmitting.

So the altimeter transmits the `state_id` values of Section 4, using the thrust flag to tell boost from coast, through a mapping from its internal states (Section 3). The mapping is the contract; the altimeter's internal state machine is its own.

---

## 3. Architecture: Two-Layer State Model

This is the most important concept in this document. The altimeter has two independent layers for flight states, and they must not be confused:

**Layer 1 — Internal State Machine:** The altimeter's own flight logic. This can have any number of states — boot sequences, calibration phases, apogee detection, separate drogue and main descent, error recovery, etc. This is the altimeter developer's domain and the ground station has no opinion on it. The more granular it is, the better the altimeter's flight logic can be.

**Layer 2 — Reported Telemetry State (`state_id`):** The single integer in the $PYRO sentence (field 2) that the tracker, DSM, and ground station all parse. This is a fixed contract with exactly 6 valid values (0–5). These values must not change — they are baked into the tracker firmware, the DSM parsers, and the ground station.

The mapping function is the bridge between these two layers; in pyro_fw it is `telemetry_state_id()` in `src/flight_outputs.c`. It takes any internal state and returns one of the 6 telemetry codes. Every internal state must have a mapping. Any unmapped state that leaks to the serial output will break the ground station.

**Apogee is an event, not a state.** Apogee is the instantaneous moment of transition from ascent to descent. It is communicated via the `$PYRO_APO` event sentence and the `APOGEE` flag bit (bit 5). There is no `state_id` value for apogee — the periodic $PYRO sentence transitions directly from state_id 1 (ASCENT) to state_id 2 (FALLING) at that moment.

### 3.1 Example: A Richer Internal State Machine

To illustrate, here is a hypothetical example of how a more granular internal state machine could map to the 6 telemetry codes. The internal states listed here are just examples — the altimeter developer is free to define whatever internal states make sense for the flight logic.

| Internal State (example) | Description | state_id | Thrust |
|---|---|---|---|
| BOOT_SETTLE | Sensor warm-up, not ready | Don't send **or** 0 | 0 |
| BOOT_CALIBRATE | Sampling ground pressure baseline | Don't send **or** 0 | 0 |
| PAD_IDLE | Ready on pad, waiting for launch | 0 | 0 |
| BOOST | Motor burning, accelerating | 1 | 1 |
| COAST | Motor burned out, still ascending | 1 | 0 |
| FALLING | Post-apogee, no chute deployed yet | 2 | 0 |
| DROGUE_DESCENT | Descending on drogue chute | 3 | 0 |
| MAIN_DESCENT | Main chute deployed, slow descent | 4 | 0 |
| LANDED | On the ground, velocity near zero | 5 | 0 |
| LANDED_LOWPOWER | Landed, reduced telemetry rate | 5 | 0 |

**Key takeaway:** The number of internal states can grow freely — add STAGING, BALLISTIC_DESCENT, ERROR_RECOVERY, or whatever the flight logic needs. The only constraint is that the mapping function must map every internal state to one of the 6 telemetry codes (0–5). The ground station never sees the internal states; it only sees what comes out of this mapping function.

**An unmapped state:** an internal state number sent on serial without the mapping reads on the ground station as an unrecognised code: the display is wrong and the flight lifecycle (Section 11) breaks.

---

## 4. Telemetry State Codes (the Fixed Contract)

The $PYRO sentence state field (field 2) must use these exact integer codes. The ground station, the DSM parsers, and the tracker firmware all key on these numbers. These 6 codes are the output of the mapping function described in Section 3.

| Code | Name | Thrust | Description | Rate |
|---|---|---|---|---|
| 0 | PAD | 0 | On the pad, pre-launch. Altimeter is idle, tracking ground pressure. | 1 Hz |
| 1 | ASCENT | 1 | **Boost phase.** Motor burning, thrust detected. GS displays this as BOOST. The thrust flag (field 3) must be 1. | 1 Hz |
| 1 | ASCENT | 0 | **Coast phase.** Motor burned out, still ascending. GS displays this as COAST. The thrust flag (field 3) must be 0. | 1 Hz |
| 2 | FALLING | 0 | Free-fall after apogee, before drogue fires. The $PYRO_APO event sentence is emitted at the moment of transition into this state. GS displays this as FALLING. | 1 Hz |
| 3 | DROGUE | 0 | Drogue chute deployed, descending under drogue. GS displays this as DROGUE. | 1 Hz |
| 4 | CHUTE | 0 | Main chute deployed, slow final descent. GS displays this as CHUTE. | 1 Hz |
| 5 | LANDED | 0 | On the ground after flight. GS displays this as LANDED and begins the landing timeout (5 minutes to auto-complete the flight). | 1 Hz |

**Key point on ASCENT:** State code 1 serves double duty. The thrust flag distinguishes boost (thrust=1) from coast (thrust=0). The ground station maps this to two separate display states: BOOST and COAST. The altimeter must set the thrust flag correctly for this mapping to work.

**Key point on FALLING:** State code 2 begins the moment apogee is detected. The $PYRO_APO event sentence must be emitted simultaneously with the first FALLING telemetry sentence (or immediately before). Apogee is never a state_id value — see Section 3.

As discussed in Section 3, the altimeter's internal state machine can have as many states as it needs. Only the `state_id` field output by the mapping function must use these 6 codes.

---

## 5. $PYRO Sentence Format

The primary telemetry sentence. Sent at the rate specified in the state table above: pyro_fw sends one a second in PAD_IDLE, in flight and in LANDED (TEL-03), and none during start-up, in FAULT or in ground test (TEL-05, DD-031). NMEA-style with XOR checksum.

```
$PYRO,seq,state,thrust,alt_cm,vel_cms,max_alt_cm,press_pa,time_ms,flags,p1_adc,p2_adc,batt,temp*XX
```

| # | Field | Type | Description |
|---|---|---|---|
| 1 | `seq` | uint16 | Sequence counter, increments each $PYRO sentence (TEL-09) |
| 2 | `state` | uint8 | Flight state code (0-5, see Section 4) |
| 3 | `thrust` | uint8 | Thrust flag: 1 = motor burning, 0 = no thrust |
| 4 | `alt_cm` | int32 | Altitude above the pad in centimeters, not clamped: below the pad reads negative (SNS-ALT-01) |
| 5 | vel_cms | int32 | Vertical velocity in centimeters/second (positive = up) |
| 6 | `max_alt_cm` | int32 | Maximum altitude reached during this flight (cm) |
| 7 | `press_pa` | int32 | Filtered barometric pressure in pascals, from the estimator the flight obeys |
| 8 | `time_ms` | uint32 | Flight time in milliseconds from T+0, the first sample of the rise, backdated from launch detection; 0 on the pad; held at its landing value in LANDED. A flight resumed after a restart counts from the resume |
| 9 | `flags` | hex | Status flags as 2-character hex (see Section 7) |
| 10 | `p1_adc` | uint16 | Pyro channel 1 ADC reading (continuity sense), the last taken on the pad |
| 11 | `p2_adc` | uint16 | Pyro channel 2 ADC reading (continuity sense), the last taken on the pad |
| 12 | batt | uint16 | Battery ADC reading (0 if not implemented; pyro_fw sends 0) |
| 13 | temp | int16 | Temperature in deci-degrees C (0 if not implemented; pyro_fw sends 0) |

pyro_fw writes decimal fields with no leading zeros and `flags` as two uppercase hex digits.

**Checksum:** Standard NMEA XOR checksum over all characters between `$` and `*` (exclusive), formatted as two uppercase hex digits after the asterisk.

**Example (pad idle):**
```
$PYRO,1,0,0,0,0,0,101325,0,03,350,348,0,0*07
```

**Example (boost):**
```
$PYRO,42,1,1,15000,28000,15000,99500,2100,13,350,348,0,0*39
```

**Example (falling — just after apogee):**
```
$PYRO,100,2,0,240000,-200,240000,75600,18500,37,350,348,0,0*11
```

**Example (drogue descent):**
```
$PYRO,150,3,0,180000,-1500,240000,81500,25000,37,350,348,0,0*2F
```

**Example (main chute descent):**
```
$PYRO,200,4,0,30000,-400,240000,97700,55000,3F,350,348,0,0*57
```

---

## 6. Event Sentences

One-shot sentences emitted for key flight events. These are in addition to the periodic $PYRO sentence. Each event is queued when it occurs and its sentence is sent exactly once, just ahead of the next periodic $PYRO sentence, oldest first, so it arrives up to 1 s after the event (TEL-11). Its fields carry the event's own altitude and flight time, not the time it was sent. The tracker uses these to generate LoRa event packets with precise altimeter-measured values.

### 6.1 $PYRO_APO (Apogee)

Sent when the altimeter detects apogee (peak altitude, transition from ascent to descent).

```
$PYRO_APO,max_alt_cm,flight_time_ms*XX
```

| Field | Description |
|---|---|
| `max_alt_cm` | Peak altitude at apogee in centimeters |
| `flight_time_ms` | Flight time at apogee in milliseconds since launch |

A flight resumed after a restart while descending (FLT-BROWN-06) enters FALLING with the APOGEE flag set and sends no $PYRO_APO.

### 6.2 $PYRO_FIRE (Pyro Channel Fired)

Sent for each pulse the flight commands on a channel: its first fire, each re-fire (PYR-REFIRE-01) and each emergency fire (FLT-EMRG-01).

```
$PYRO_FIRE,channel,alt_cm,flight_time_ms*XX
```

| Field | Description |
|---|---|
| `channel` | 1 = pyro channel 1, 2 = pyro channel 2 |
| `alt_cm` | Altitude at firing in centimeters |
| `flight_time_ms` | Flight time at firing in milliseconds since launch |

### 6.3 $PYRO_LAND (Landing)

Sent when the altimeter detects landing (vertical velocity near zero after descent).

```
$PYRO_LAND,max_alt_cm,flight_time_ms*XX
```

| Field | Description |
|---|---|
| `max_alt_cm` | Maximum altitude from the flight in centimeters |
| `flight_time_ms` | Total flight time at landing in milliseconds since launch |

All event sentences use the same NMEA XOR checksum as $PYRO. The tracker parses these independently from the periodic $PYRO sentence. Event sentences provide the authoritative altimeter-measured values for the event timeline; the periodic $PYRO sentence continues at its normal rate.

Field orders are specified in Sections 6.1-6.3 above; pyro_fw sends them in these orders (`src/telemetry.c`).

---

## 7. Flags Bitfield

The flags field (field 9) is a hex-encoded bitfield. pyro_fw sets bits 0 through 5 and transmits them as 2-character hex, e.g. `3F`. The tracker receives and forwards 16-bit extended flags (4-character hex); pyro_fw sets none of bits 6-11.

### 7.1 Base Flags (bits 0-5) — Currently Implemented

| Bit | Name | Hex Mask | Meaning |
|---|---|---|---|
| 0 | P1_CONT | `0x01` | Pyro channel 1 continuity detected (e-match connected) |
| 1 | P2_CONT | `0x02` | Pyro channel 2 continuity detected |
| 2 | P1_FIRED | `0x04` | Pyro channel 1 has fired: the flight has commanded a pulse on it |
| 3 | P2_FIRED | `0x08` | Pyro channel 2 has fired |
| 4 | ARMED | `0x10` | Pyro system is armed |
| 5 | APOGEE | `0x20` | Apogee has been detected, or a resumed flight took it as passed |

pyro_fw reads continuity on the pad. From launch, bits 0 and 1, like `p1_adc` and `p2_adc`, hold the last pad reading.

### 7.2 Extended Flags (bits 6-11) — Future / Optional

The tracker firmware detects and relays these extended flags. pyro_fw does not set them. An altimeter that sets any of these bits transmits the flags field as 4-character hex (e.g. `023F`) instead of 2-character.

| Bit | Name | Hex Mask | Meaning |
|---|---|---|---|
| 6 | P1_FAIL | `0x0040` | Pyro 1 failure declared by altimeter |
| 7 | P2_FAIL | `0x0080` | Pyro 2 failure declared |
| 8 | DROGUE_OK | `0x0100` | Drogue deployment confirmed |
| 9 | DROGUE_FAIL | `0x0200` | Drogue deployment failed |
| 10 | MAIN_OK | `0x0400` | Main chute deployment confirmed |
| 11 | MAIN_FAIL | `0x0800` | Main chute deployment failed |

The tracker's hex parser handles both 2-character and 4-character hex.

---

## 8. Serial Interface

The altimeter board connects to the tracker board over a wired UART link.

| Parameter | Value |
|---|---|
| Baud rate | 115200 |
| Format | 8N1 (8 data bits, no parity, 1 stop bit) |
| Line termination | CR+LF or LF; pyro_fw sends CR+LF |
| Protocol | NMEA-style sentences (`$` prefix, `*` checksum delimiter) |
| Output format | NMEA `$PYRO` sentences. pyro_fw has no other format (TEL-01); a different one is a Lua script on the serial pins (TEL-12). |

pyro_fw's port accepts no commands (TEL-12). The same UART also carries diagnostic lines that begin with `!` (`!FAULT` every 5 s from a faulted board, `!GT` in ground test, `!SENSOR STUCK` and others) and web-server debug lines that begin with `HTTP:`. Neither begins with `$`.

---

## 9. Required Altimeter State Mapping

`flight_state_t` (`src/flight_states.h`) has twelve states, and `telemetry_state_id()` in `src/flight_outputs.c` maps the six that send a $PYRO sentence (TEL-07); the boot states, FAULT and GROUND_TEST send none (TEL-05, DD-031).

| Internal State | state_id | GS Interprets As | Note |
|---|---|---|---|
| `PAD_IDLE` | 0 | PAD | |
| `ASCENT` | 1 | BOOST or COAST | The thrust flag tells them apart |
| `FALLING` | 2 | FALLING | |
| `DROGUE_DESCENT` | 3 | DROGUE | |
| `CHUTE_DESCENT` | 4 | CHUTE | |
| `LANDED` | 5 | LANDED | |
| `BOOT_SETTLE`, `BOOT_SENSOR`, `BOOT_CONTINUITY`, `BOOT_CALIBRATE`, `FAULT`, `GROUND_TEST` | — | — | No $PYRO sentence |

The three descent states:

- **FALLING** (state_id 2): entered at apogee detection, with the `$PYRO_APO` event sentence. The rocket is in free-fall, or faster than any canopy's band.
- **DROGUE** (state_id 3): the measured descent rate holds steady in the drogue's band.
- **CHUTE** (state_id 4): the measured descent rate holds steady in the main chute's band.

pyro_fw names the descent state from the measured descent rate, whichever channel fired (FLT-DESC-01), and returns from DROGUE_DESCENT to FALLING when a drogue stops slowing the rocket: state_id can go from 3 back to 2. Landing is detected in every descent state (FLT-DESC-02), so a flight with no canopy goes from 2 straight to 5 (`docs/flight_states.md`).

---

## 10. Required Changes for Compatibility

An altimeter is compatible when it meets the items below. pyro_fw meets each (`src/telemetry.c`, `src/flight_outputs.c`; DD-088):

- 10.1: Section 9.
- 10.2: the thrust flag is 1 in ASCENT while the pressure fit shows upward acceleration, so it falls to 0 at burnout, and it is 0 in every other state (TEL-10).
- 10.3: `$PYRO_APO,max_alt_cm,flight_time_ms`, `$PYRO_FIRE,channel,alt_cm,flight_time_ms` and `$PYRO_LAND,max_alt_cm,flight_time_ms`, as Sections 6.1-6.3.
- 10.4: NMEA only (TEL-01).

### 10.1 The State Mapping

The mapping function must map ALL internal states to the 6 valid $PYRO codes (0–5), as described in Section 3. Any unmapped state that leaks to the serial output will break the ground station display.

Mapping rules:

- Any boot/calibration state → Do not send telemetry (or send state_id 0)
- Any ascending/coasting state → state_id 1 (ASCENT), with thrust flag set appropriately
- Post-apogee free-fall (before drogue) → state_id 2 (FALLING)
- Descending on drogue chute → state_id 3 (DROGUE)
- Descending on main chute → state_id 4 (CHUTE)
- Any ballistic/uncontrolled descent → state_id 2 (FALLING)
- Any post-landing state → state_id 5 (LANDED)

### 10.2 Verify Thrust Flag Transitions

The thrust flag must transition from 1 to 0 during ascent when motor burnout is detected. This transition generates the BURNOUT event on the tracker. If the thrust flag is always 0, the ground station will never show a BOOST phase and the burnout event will not fire.

### 10.3 Verify Event Sentence Field Order

The tracker firmware parses the fields in the order specified in this document (Sections 6.1-6.3). pyro_fw's `src/telemetry.c` and its README give the same order.

### 10.4 Default Telemetry Format

pyro_fw sends NMEA only (TEL-01); the tracker cannot parse JSON.

---

## 11. Ground Station Flight Lifecycle

For context, the ground station manages flights through this state machine. The altimeter does not need to implement this, but understanding it helps explain why correct state_id values matter.

| From | To | Trigger | Timeout |
|---|---|---|---|
| PREFLIGHT | ACTIVE | Non-PAD telemetry received | — |
| ACTIVE | RECOVERY | No telemetry for 60 seconds | 60s |
| RECOVERY | ACTIVE | Telemetry resumes | — |
| ACTIVE/RECOVERY | COMPLETE | Telemetry shows state_id=5 (LANDED) for **5 continuous minutes** | 300s |
| ACTIVE/RECOVERY | COMPLETE | Operator manually marks complete | — |

**Why LANDED state_id matters:** The ground station's auto-complete logic checks specifically for `flightState === 5` (after DSM mapping). If the altimeter sends an unrecognized state code when landed, the flight will never auto-complete. It will eventually transition to RECOVERY (no telemetry for 60s) and stay stuck there until an operator manually completes it.

---

## 12. Tracker Event Code Reference

The tracker generates LoRa event packets when it detects state or flag transitions from the altimeter. These events populate the flight timeline on the ground station. This section documents the event codes so the altimeter developer understands what downstream effects each flag/state transition produces.

| Code | Name | Triggered By | Source |
|---|---|---|---|
| `'B'` | **BURNOUT** | Thrust flag transitions from 1 to 0 | $PYRO |
| `'A'` | **APOGEE** | `$PYRO_APO` sentence, or FLAG_APOGEE (bit 5) newly set | Both |
| `1` | **P1_FIRE** | `$PYRO_FIRE` (ch=1), or FLAG_P1_FIRED (bit 2) newly set | Both |
| `2` | **P1_FAIL** | EFLAG_P1_FAIL (bit 6) newly set | Flags |
| `3` | **DROGUE_OK** | EFLAG_DROGUE_OK (bit 8) newly set | Flags |
| `4` | **DROGUE_FAIL** | EFLAG_DROGUE_FAIL (bit 9) newly set | Flags |
| `5` | **P2_FIRE** | `$PYRO_FIRE` (ch=2), or FLAG_P2_FIRED (bit 3) newly set | Both |
| `6` | **P2_FAIL** | EFLAG_P2_FAIL (bit 7) newly set | Flags |
| `7` | **MAIN_OK** | EFLAG_MAIN_OK (bit 10) newly set | Flags |
| `8` | **MAIN_FAIL** | EFLAG_MAIN_FAIL (bit 11) newly set | Flags |
| `'L'` | **LANDED** | `$PYRO_LAND` sentence, or state transitions to LANDED (5) | Both |
| `'R'` | **REPORT** | 5 seconds after landing (max altitude, max velocity, errors) | $PYRO |

Events marked **Both** can be triggered by either an event sentence or a flag transition. The tracker deduplicates: if a `$PYRO_APO` sentence fires the APOGEE event, the subsequent FLAG_APOGEE transition is suppressed (and vice versa). The altimeter should send event sentences for the most accurate timing; the flag-based detection serves as a fallback.

---

## 13. Quick Reference for Altimeter Developers

1. The $PYRO `state_id` field must be 0, 1, 2, 3, 4, or 5. No other values.
2. Thrust flag = 1 during motor burn, 0 otherwise. This creates the BOOST vs COAST distinction within state_id 1.
3. Apogee is an event, not a state. Emit `$PYRO_APO` at the moment of apogee; the state_id transitions from 1 (ASCENT) to 2 (FALLING).
4. Descent is three states: FALLING (2) = free-fall before drogue; DROGUE (3) = drogue deployed; CHUTE (4) = main chute deployed.
5. Ballistic or uncontrolled descent = state_id 2 (FALLING).
6. Landed = state_id 5. The ground station auto-completes after 5 minutes of continuous state_id 5.
7. Emit `$PYRO_APO`, `$PYRO_FIRE`, `$PYRO_LAND` event sentences at the appropriate moments.
8. Flags bits 0-5 must be maintained correctly. The tracker watches for 0→1 transitions.
9. The output is NMEA. The tracker cannot parse JSON.
10. 1 Hz in every state. An event sentence is sent with the next periodic sentence, up to 1 s after its event.
