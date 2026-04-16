---
date: 2026-04-16T01:00:38Z
researcher: Joseph Julicher
git_commit: 5019cf2c5d8f45adee91f3299a231ad8b659f085
branch: main
repository: pyro_fw
topic: "Current telemetry format vs ground-station-interface-spec"
tags: [research, codebase, telemetry, nmea, flight-states, pyro-protocol]
status: complete
last_updated: 2026-04-15
last_updated_by: Joseph Julicher
---

# Research: Current Telemetry Format vs Ground Station Interface Spec

**Date**: 2026-04-16T01:00:38Z  
**Researcher**: Joseph Julicher  
**Git Commit**: 5019cf2c5d8f45adee91f3299a231ad8b659f085  
**Branch**: main  
**Repository**: pyro_fw

## Research Question

Compare the current telemetry format in the firmware with the ground-station-interface-spec (docs/ground-station-interface-spec.md).

## Summary

The firmware's $PYRO telemetry protocol is **largely compliant** with the spec. All state codes, event sentences, flags bits, serial parameters, and transmission rates match exactly. There are two gaps: `batt` and `temp` (fields 12-13) are hardcoded to `0` rather than being wired to hardware sensors. The `thrust` flag uses an acceleration-proxy (`vertical_speed_cms > prev_vertical_speed_cms`) rather than direct motor-burn detection, but this produces the correct 1→0 transition at burnout.

---

## Detailed Findings

### $PYRO Sentence Format Comparison

**Spec** (`docs/ground-station-interface-spec.md`, Section 5):
```
$PYRO,seq,state,thrust,alt_cm,vel_cms,max_alt_cm,press_pa,time_ms,flags,p1_adc,p2_adc,batt,temp*XX
```

**Firmware** (`src/telemetry_formatter.c:63-67`):
```c
snprintf(payload, sizeof(payload),
    "PYRO,%u,%u,%u,%ld,%ld,%ld,%ld,%lu,%02X,%u,%u,%d,%d",
    s->seq, s->state_id, s->thrust,
    s->alt_cm, s->speed_cms, s->max_alt_cm, s->press_pa, s->time_ms,
    s->flags, s->p1_adc, s->p2_adc,
    0,   /* batt — hardcoded */
    0);  /* temp — hardcoded */
```

Wire output: `$PYRO,seq,st,thr,alt,spd,max,pa,ms,fl,a1,a2,0,0*XX\r\n`

#### Field-by-Field Comparison

| # | Spec field | Spec type | Firmware struct field | Firmware type | Status |
|---|---|---|---|---|---|
| 1 | `seq` | uint16 | `s->seq` (`ctx->telemetry_seq`) | uint16_t | ✅ Match |
| 2 | `state` | uint8 (0-3) | `s->state_id` from `state_to_telem_id()` | uint8_t | ✅ Match |
| 3 | `thrust` | uint8 (0/1) | `s->thrust` (see thrust section) | uint8_t | ✅ Match (see note) |
| 4 | `alt_cm` | int32 | `s->alt_cm` = `ctx->last_altitude` | int32_t | ✅ Match |
| 5 | `vel_cms` | int32, positive=up | `s->speed_cms` = `ctx->vertical_speed_cms` | int32_t | ✅ Match |
| 6 | `max_alt_cm` | int32 | `s->max_alt_cm` = `ctx->max_altitude` | int32_t | ✅ Match |
| 7 | `press_pa` | int32 | `s->press_pa` = `ctx->filtered_pressure` | int32_t | ✅ Match |
| 8 | `time_ms` | uint32, 0 on pad | `s->time_ms` (0 while PAD_IDLE) | uint32_t | ✅ Match |
| 9 | `flags` | hex (2-char) | `s->flags`, printed as `%02X` | uint8_t | ✅ Match |
| 10 | `p1_adc` | uint16 | `s->p1_adc` = `ctx->pyro1_adc` | uint16_t | ✅ Match |
| 11 | `p2_adc` | uint16 | `s->p2_adc` = `ctx->pyro2_adc` | uint16_t | ✅ Match |
| 12 | `batt` | uint16, 0 if not impl. | hardcoded literal `0` | int | ⚠️ Hardcoded 0, no struct field |
| 13 | `temp` | int16, 0 if not impl. | hardcoded literal `0` | int | ⚠️ Hardcoded 0, no struct field |

---

### State Codes (Spec Section 4)

`state_to_telem_id()` is defined at `src/flight_states.c:691-704`.

| Internal `flight_state_t` | `state_to_telem_id()` output | Spec expects | Status |
|---|---|---|---|
| `PAD_IDLE` | 0 | 0 (PAD) | ✅ Match |
| `ASCENT` | 1 | 1 (ASCENT) | ✅ Match |
| `DESCENT` | 2 | 2 (DESCENT) | ✅ Match |
| `LANDED` | 3 | 3 (LANDED) | ✅ Match |
| `BOOT_SETTLE` | 0 (default) | don't send or 0 | ✅ Match — telemetry suppressed by `>= PAD_IDLE` guard at `flight_states.c:710` |
| `BOOT_CONTINUITY` | 0 (default) | don't send or 0 | ✅ Match — same guard |
| `BOOT_CALIBRATE` | 0 (default) | don't send or 0 | ✅ Match — same guard |

The `>= PAD_IDLE` check at `flight_states.c:710` means the boot states never reach `state_to_telem_id()` at runtime. The `default: return 0` is a safety fallback.

---

### Thrust Flag (Spec Section 4 — "1 = motor burning, 0 = no thrust")

**Spec definition**: thrust=1 when motor burning (boost), thrust=0 otherwise (coast, descent, pad, landed).

**Firmware implementation** (`src/flight_states.c:333`, `flight_update_outputs():717`):
```c
// In detect_ascent() — runs every cycle while state == ASCENT:
ctx->under_thrust = ctx->vertical_speed_cms > ctx->prev_vertical_speed_cms;

// In flight_update_outputs() — telemetry snapshot:
.thrust = (ctx->current_state == ASCENT && ctx->under_thrust) ? 1u : 0u,
```

`under_thrust` is an acceleration proxy: it is `true` when vertical speed this sample exceeds the previous sample (i.e., the rocket is still accelerating). It becomes `false` the instant speed stops increasing — corresponding to burnout/coast.

The thrust flag is forced to `0` in all states other than `ASCENT`, satisfying the spec requirement that thrust=0 on pad, during descent, and after landing.

The spec's required 1→0 transition during ascent at burnout is produced by the acceleration proxy correctly.

---

### Event Sentences (Spec Section 6)

All three event sentences are fully implemented in `src/telemetry_formatter.c`.

#### $PYRO_APO
- **Spec**: `$PYRO_APO,max_alt_cm,flight_time_ms*XX`
- **Firmware** (`telemetry_formatter.c:91`): format `"PYRO_APO,%ld,%lu"` → fields: `e->max_alt_cm`, `e->flight_time_ms`
- **Trigger**: `action_apogee()` at `flight_states.c:490`
- **Status**: ✅ Field order matches spec

#### $PYRO_FIRE
- **Spec**: `$PYRO_FIRE,channel,alt_cm,flight_time_ms*XX`
- **Firmware** (`telemetry_formatter.c:110`): format `"PYRO_FIRE,%u,%ld,%lu"` → fields: `e->channel`, `e->alt_cm`, `e->time_ms`
- **Trigger**: `try_fire_pyros()` at `flight_states.c:99,107`
- **Status**: ✅ Field order matches spec

#### $PYRO_LAND
- **Spec**: `$PYRO_LAND,max_alt_cm,flight_time_ms*XX`
- **Firmware** (`telemetry_formatter.c:130`): format `"PYRO_LAND,%ld,%lu"` → fields: `e->max_alt_cm`, `e->flight_time_ms`
- **Trigger**: `action_landing()` at `flight_states.c:500`
- **Status**: ✅ Field order matches spec

> **Note**: The spec (Section 10.3) calls out a "known documentation error" in the altimeter README where field orders differ. The firmware's actual format strings above match the spec, not whatever the README says.

---

### Flags Bitfield (Spec Section 7)

All six base flags (bits 0-5) are implemented. Extended flags (bits 6-11) are not set by the firmware — the spec marks them "Future / Optional."

| Spec name | Spec mask | Firmware macro | Firmware mask | Source bool | Status |
|---|---|---|---|---|---|
| `P1_CONT` | 0x01 | `TELEM_FLAG_P1_CONT` | 0x01 | `ctx->pyro1_continuity_good` | ✅ Match |
| `P2_CONT` | 0x02 | `TELEM_FLAG_P2_CONT` | 0x02 | `ctx->pyro2_continuity_good` | ✅ Match |
| `P1_FIRED` | 0x04 | `TELEM_FLAG_P1_FIRED` | 0x04 | `ctx->pyro1_fired` | ✅ Match |
| `P2_FIRED` | 0x08 | `TELEM_FLAG_P2_FIRED` | 0x08 | `ctx->pyro2_fired` | ✅ Match |
| `ARMED` | 0x10 | `TELEM_FLAG_ARMED` | 0x10 | `ctx->pyros_armed` | ✅ Match |
| `APOGEE` | 0x20 | `TELEM_FLAG_APOGEE` | 0x20 | `ctx->apogee_detected` | ✅ Match |

Flags are transmitted as `%02X` (2-character uppercase hex) — matches spec.

Extended flags (bits 6-11: `P1_FAIL`, `P2_FAIL`, `DROGUE_OK`, `DROGUE_FAIL`, `MAIN_OK`, `MAIN_FAIL`) are defined in the spec but not in the firmware. No struct fields or macros exist for these. The tracker's `parse_hex_word()` handles both 2- and 4-character hex per the spec; the firmware's 2-character output is fully compatible.

---

### Serial Interface (Spec Section 8)

| Spec requirement | Firmware implementation | Location | Status |
|---|---|---|---|
| 115200 baud | `uart_init(uart0, 115200)` | `hal_hardware.c:595` | ✅ Match |
| 8N1 | RP2040 UART default | — | ✅ Match |
| CR+LF or LF | `\r\n` appended in `nmea_send()` | `telemetry_formatter.c:42` | ✅ Match |
| NMEA-style `$` prefix, `*` checksum | Implemented in `nmea_send()` | `telemetry_formatter.c:37-44` | ✅ Match |
| `telem_format` defaults to 0 (NMEA) | `X(U8, telem_format, "telem_format", 0)` | `config_fields.h:35` | ✅ Match |

---

### Transmission Rates (Spec Section 4)

| State | Spec rate | Firmware interval | Firmware rate | Status |
|---|---|---|---|---|
| PAD | 1 Hz | 1000 ms | 1 Hz | ✅ Match |
| ASCENT | 10 Hz | 100 ms | 10 Hz | ✅ Match |
| DESCENT | 10 Hz | 100 ms | 10 Hz | ✅ Match |
| LANDED | 1 Hz | 1000 ms | 1 Hz | ✅ Match |
| Boot states | (no telemetry) | suppressed by `>= PAD_IDLE` | — | ✅ Match |

Rate logic is in `flight_states.c:710-712`:
```c
uint32_t interval = (ctx->current_state == ASCENT || ctx->current_state == DESCENT) ? 100 : 1000;
if (now - ctx->last_telemetry >= interval) { ... }
```

Note: The `telem_rate_hz` config field (`config_fields.h:36`, default 10) exists but is not used in this rate calculation. The hardcoded 100/1000 ms values govern actual transmission.

---

## Code References

- `src/telemetry_formatter.c:63-67` — $PYRO sentence snprintf format string
- `src/telemetry_formatter.c:37-44` — `nmea_send()`: NMEA wrapping and XOR checksum
- `src/telemetry_formatter.c:91` — `$PYRO_APO` format string
- `src/telemetry_formatter.c:110` — `$PYRO_FIRE` format string
- `src/telemetry_formatter.c:130` — `$PYRO_LAND` format string
- `src/telemetry_formatter.h:36-41` — Flags macro definitions (bits 0-5)
- `src/telemetry_formatter.h:49-61` — `telemetry_snapshot_t` struct (no batt/temp fields)
- `src/flight_states.c:691-704` — `state_to_telem_id()` mapping function
- `src/flight_states.c:710-742` — `flight_update_outputs()`: rate gating and snapshot build
- `src/flight_states.c:333` — `under_thrust` acceleration proxy
- `src/flight_states.h:11-20` — `flight_state_t` enum (7 values + sentinel)
- `src/hal_hardware.c:595` — UART0 init at 115200 baud
- `src/config_fields.h:35` — `telem_format` default 0 (NMEA)

## Architecture Documentation

The telemetry pipeline is: `flight_update_outputs()` (flight_states.c) → populates `telemetry_snapshot_t` → calls `telemetry_state()` (telemetry_formatter.c) → builds NMEA string via `snprintf` + `nmea_send()` → calls `hal_telemetry_send()` → ISR-driven 512-byte ring buffer → UART0 GPIO0 → 115200 baud.

The format/NMEA vs JSON split is controlled by `telem_format` in `config_t`, checked inside each `telemetry_*()` function in `telemetry_formatter.c`. JSON variants exist for all four sentence types but are never used in production (default is NMEA).

## Gap Summary

| Gap | Location | Spec expectation | Current state |
|---|---|---|---|
| `batt` field | `telemetry_formatter.c:67` | uint16 ADC reading, 0 if not implemented | Hardcoded literal `0`; no struct field |
| `temp` field | `telemetry_formatter.c:67` | int16 deci-°C, 0 if not implemented | Hardcoded literal `0`; no struct field |
| Extended flags (bits 6-11) | `telemetry_formatter.h` | Future/optional, 4-char hex if set | Not defined; spec explicitly marks these optional |
| `telem_rate_hz` config unused | `flight_states.c:710-712` | N/A — spec doesn't address this config field | Config field exists but rate is hardcoded |

## Open Questions

- Whether a barometric temperature reading from the pressure sensor (e.g., BMP280/MS5607 provides temp) should be wired to the `temp` field.
- Whether battery voltage sensing hardware exists on the board and should be wired to `batt`.
