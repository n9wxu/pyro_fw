# Requirements Traceability Matrix

Requirements are verified through integration and closed-loop tests that exercise
the complete system through flight scenarios. Unit tests verify implementation
correctness but are not directly linked to requirements.

## Verification Tests

### Integration Tests (test_integration.c)
Simulate a complete flight using OpenRocket trajectory data at 1ms resolution.

### Closed-Loop Tests (test_closedloop.c)
Simulate flights with physics feedback across 7 pyro configurations × 4 altitudes.

### Web UI Tests (test_ui.spec.js)
Verify web interface behavior against mock server in 3 device modes.

---

## 1. Recovery Deployment

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| UN-1 | Deploy recovery devices safely | All closed-loop tests | ✅ |
| SYS-DEPLOY-01 | Fire pyros at configurable events | Closed-loop: all 7 config suites | ✅ |
| SYS-DEPLOY-02 | Two independent channels | Closed-loop: both channels fire in all suites | ✅ |
| SYS-DEPLOY-03 | No firing before leaving rail | — | ⚠️ |
| FLT-PHASE-01 | Detect launch | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-PHASE-02 | Detect apogee | Integration: test_FLT_APO_01_detected | ✅ |
| FLT-PHASE-03 | Detect landing | Integration: test_FLT_LAND_04_duration | ✅ |
| PYR-MODE-01 | DELAY mode | Closed-loop: test_PYR_MODE_01_delay_delay | ✅ |
| PYR-MODE-02 | AGL mode | Closed-loop: test_PYR_MODE_02_delay_agl, test_PYR_MODE_02_agl_agl | ✅ |
| PYR-MODE-03 | FALLEN mode | Closed-loop: test_PYR_MODE_03_delay_fallen, test_PYR_MODE_03_fallen_agl | ✅ |
| PYR-MODE-04 | SPEED mode | Closed-loop: test_PYR_MODE_04_delay_speed, test_PYR_MODE_04_speed_agl | ✅ |
| PYR-SAFE-01 | No fire without continuity | — | ⚠️ |
| PYR-SAFE-02 | No simultaneous fire | — | ⚠️ |
| PYR-SAFE-03 | Single fire per channel | Closed-loop: verified by fire_count | ✅ |
| PYR-SAFE-04 | No fire before apogee | Closed-loop: drogue fires at/after apogee | ✅ |
| FLT-LAUNCH-01 | Transition at >10m | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-LAUNCH-02 | Stay at ground level | Integration: PAD_IDLE persists before launch | ✅ |
| FLT-LAUNCH-03 | Backdate launch time | — | ⚠️ |
| FLT-LAUNCH-04 | Log LAUNCH event | Integration: test_DAT_04_events | ✅ |
| FLT-LAUNCH-05 | Stop buzzer on launch | Integration: test_BUZ_07_03_lifecycle | ✅ |
| FLT-APO-01 | Apogee when speed ≤ 0 | Integration: test_FLT_APO_01_detected | ✅ |
| FLT-APO-02 | Transition to DESCENT | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-APO-03 | Log APOGEE event | Integration: test_DAT_04_events | ✅ |
| FLT-APO-04 | No apogee before armed | — | ⚠️ |
| FLT-ASC-01 | Track max altitude | Integration: test_FLT_APO_01_detected (max_altitude > 0) | ✅ |
| FLT-ASC-02 | Compute vertical speed | Integration: apogee detection depends on speed | ✅ |
| FLT-ASC-03 | Detect thrust phase | — | ⚠️ |
| FLT-ASC-04 | Arm at <10 m/s | Closed-loop: pyros arm and fire in all flights | ✅ |
| FLT-ASC-05 | Log ARMED event | Integration: test_DAT_04_events | ✅ |
| FLT-ASC-06 | No arm above 10 m/s | — | ⚠️ |
| FLT-LAND-01 | Stable <1m for 1s | Integration: test_FLT_BOOT_01_all_states reaches LANDED | ✅ |
| FLT-LAND-02 | Speed <2 m/s | Integration: landing detected at correct time | ✅ |
| FLT-LAND-03 | Altitude <30m | Integration: landing detected at correct time | ✅ |
| FLT-LAND-04 | Transition to LANDED | Integration: test_FLT_LAND_04_duration | ✅ |
| FLT-LAND-05 | Log LANDING event | Integration: test_DAT_04_events | ✅ |
| FLT-LAND-06 | Stay in LANDED | Integration: state remains LANDED after detection | ✅ |
| PYR-REFIRE-01 | Re-fire if ballistic | — | ⚠️ |
| PYR-ALT-01 | Clamp altitude settings | — | ⚠️ |
| PYR-ALT-02 | Warning beep for range | — | ⚠️ |
| FLT-RATE-01..04 | Sample rates | Integration: test_FLT_LAUNCH_01_timing (timing bounds) | ⚠️ |

## 2. Pre-Flight Status

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-STATUS-01 | Audible readiness | Integration: test_BUZ_07_03_lifecycle | ✅ |
| SYS-STATUS-02 | Verify pyro integrity | Integration: continuity checked before flight | ✅ |
| PYR-CONT-01 | Check every 1s | — | ⚠️ |
| PYR-CONT-02 | Report good/open/short | Web UI: pyro channels show OK/OPEN/FIRED | ✅ |
| BUZ-STATUS-01 | Distinct beep codes | — | ⚠️ |
| BUZ-01..02 | Startup chirps + code | — | ⚠️ |
| FLT-BOOT-01 | Non-blocking boot | Integration: test_FLT_BOOT_01_all_states | ✅ |
| FLT-BOOT-02..03 | Config read/create | — | ⚠️ |
| FLT-BOOT-04 | Settle wait | Integration: boot completes in expected time | ✅ |
| FLT-BOOT-08 | Calibrate from 10 readings | Integration: ground pressure established | ✅ |
| FLT-BOOT-09 | 2s stabilization | — | ⚠️ |

## 3. Flight Data Recovery

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-DATA-01 | Record flight data | Integration: test_DAT_04_events (samples > 100) | ✅ |
| SYS-DATA-02 | Export standard format | — | ⚠️ |
| SYS-DATA-03 | Announce max altitude | Integration: test_BUZ_07_03_lifecycle | ✅ |
| DAT-01 | 4096-entry ring buffer | Integration: samples recorded throughout flight | ✅ |
| DAT-02 | Sample fields | Integration: events have correct fields | ✅ |
| DAT-03 | Events tag samples | Integration: test_DAT_04_events | ✅ |
| DAT-04 | Log all event types | Integration: test_DAT_04_events | ✅ |
| DAT-06 | CSV export | — | ⚠️ |
| DAT-07 | CSV metadata header | — | ⚠️ |
| BUZ-03..07 | Altitude beep-out | Integration: test_BUZ_07_03_lifecycle | ✅ |

## 4. Configuration

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-CFG-01 | Persistent config | Web UI: config persists across reboot | ✅ |
| SYS-CFG-02 | Config without tools | Web UI: config editor tests | ✅ |
| SYS-CFG-03 | Validate against limits | Web UI: range warning test | ✅ |
| CFG-01..09 | INI parsing | Closed-loop: all configs parsed and applied correctly | ✅ |
| WEB-UI-02 | Guided editor | Web UI: config tab tests | ✅ |
| WEB-UI-03 | Warn if not applied | Web UI: save shows confirmation | ✅ |

## 5. Altitude Measurement

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-ALT-01 | Barometric altitude | Integration: altitude tracks trajectory | ✅ |
| SYS-ALT-02 | Multiple sensors | — (hardware test only) | ⚠️ |
| SNS-PRES-01 | Auto-detect sensor | — (hardware test only) | ⚠️ |
| SNS-PRES-02..04 | Pressure filter | Integration: altitude converges correctly | ✅ |
| SNS-ALT-01..03 | Altitude computation | Integration: max altitude within expected range | ✅ |

## 6. Telemetry

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| SYS-TEL-01 | Serial telemetry | Integration: test_TEL_01_output | ✅ |
| TEL-01..02 | NMEA format + checksum | Integration: test_TEL_01_output (checksum verified) | ✅ |
| TEL-03..05 | Telemetry rates | Integration: ≥10 sentences during flight | ✅ |
| TEL-06..10 | Field contents | Integration: test_TEL_01_output | ✅ |

## 7. Pyro Fault Protection

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| PYR-FAULT-01 | Disable at >1.5A | N/A (hardware) | ✅ HW |
| PYR-FAULT-02 | Detect overcurrent | — | ❌ |
| PYR-FAULT-03 | Indicate overcurrent | — | ❌ |
| PYR-VERIFY-01 | Post-fire verification | — | ❌ |

## 8. Web Interface & Network

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| WEB-NET-01..04 | USB network, DHCP, mDNS, DNS-SD | Hardware: support/test_network.py | ⚠️ HW |
| WEB-API-01 | GET /api/status | Web UI: status tests (3 modes) | ✅ |
| WEB-API-02 | GET /api/config | Web UI: config loads from device | ✅ |
| WEB-API-03 | POST /api/config | Web UI: save test | ✅ |
| WEB-API-04 | POST /api/ota | — | ⚠️ HW |
| WEB-API-05 | POST /api/reboot | — | ⚠️ |
| WEB-API-06 | GET /api/flight.csv | — (stub) | ⚠️ |
| WEB-API-07 | CORS headers | — | ⚠️ |
| WEB-UI-01 | Status in config units | Web UI: altitude in meters/feet tests | ✅ |
| WEB-UI-04 | Flight summary + CSV | Web UI: flight data tests | ✅ |
| WEB-UI-05 | Firmware upload | Web UI: update tab test | ✅ |

## 9. Firmware Update

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| OTA-01..04 | OTA, inactive slot, rollback, safety | — (hardware test only) | ⚠️ HW |

## 10. Portability & Testability

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| HAL-01 | No #ifdef in flight code | CI: grep verification | ✅ |
| HAL-02..04 | HAL interface | CI: all 3 targets build from same source | ✅ |

## 11. Build & Test System

| Req | Description | Verified By | Status |
|-----|-------------|-------------|--------|
| BLD-01..05 | Build targets | CI: build succeeds | ✅ |
| TST-01..03 | Unit/integration/closed-loop | CI: all pass | ✅ |
| TST-04 | All 4 pyro modes | Closed-loop: 7 config suites | ✅ |
| TST-05 | 100ft to 100km | Closed-loop: 4 altitude profiles | ✅ |
| TST-06 | Chute reduces descent | Closed-loop: test_TST_06_chute_effect | ✅ |
| TST-07 | Web UI tests | CI: Playwright tests | ✅ |
| TST-08 | CI on every push | GitHub Actions | ✅ |
| TST-09 | Traceable to requirements | This document | ✅ |

---

## Summary

| Status | Count |
|--------|-------|
| ✅ Verified by integration/closed-loop/web test | 74 |
| ⚠️ Not directly verified (needs integration test or hardware) | 28 |
| ❌ Not implemented | 3 |
| ✅ HW (hardware satisfies) | 1 |

### Critical gaps needing integration tests:
1. **PYR-SAFE-01**: No fire without continuity — add closed-loop flight with bad continuity
2. **PYR-SAFE-02**: No simultaneous fire — add closed-loop flight verifying fire_count timing
3. **PYR-REFIRE-01**: Re-fire logic — add closed-loop flight with failed first deployment
4. **SYS-DEPLOY-03**: No firing before rail departure — add integration test with low-altitude noise

### Not implemented:
5. **PYR-FAULT-02/03**: Overcurrent detection and indication
6. **PYR-VERIFY-01**: Post-fire continuity verification
