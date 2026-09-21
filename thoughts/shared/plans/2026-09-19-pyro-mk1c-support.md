# Pyro MK1C Hardware Support Implementation Plan

## Overview

Add support for the `pyro_mk1c` board alongside the existing MK1B target. MK1C is a
bare-RP2040 design with a completely different pyrotechnic architecture: a dynamic
charge-pump arm element (TPS259570 eFuse), bias-injection continuity sensing, four
analog sense channels, and a capacitor-discharge firing pulse. The firing behaviour is
specified in two documents that live with the board design, not in this repo:

- `~/Documents/pyro_mk1c/DESIGN.md` (draft 0.5) — hardware, numbers, FMEA, invariants
- `~/Documents/pyro_mk1c/IGNITER_OPERATION.md` (draft 0.1) — S0–S8 / F0–F10 state machines

MK1B must stay buildable. Board selection is a build-time CMake option; there is no
board ID pin and no runtime detection.

**`src/hal.h` does not change, and `src/pyro.h` does not change.** MK1C is a new
*implementation* behind the existing contracts, not an extension of them. `src/pyro.c`
and `src/pressure_sensor.c` are not refactored, not parameterised, and not ported —
they stay exactly as they are and become the MK1B-only implementations. CMake compiles
one board's set or the other's.

## Current State Analysis

The seam this port needs already exists. `hal_hardware.c:294-322` implements the whole
`hal_pyro_*` surface by delegating to `pyro.h`:

```c
void hal_pyro_init(void)                  { pyro_init(); }
void hal_pyro_check(p1, p2)               { pyro_check_continuity(&c1, &c2); /* copy */ }
void hal_pyro_fire(uint8_t channel)       { pyro_fire(channel); }
void hal_pyro_update(uint32_t now_ms)     { pyro_update(now_ms); }
bool hal_pyro_is_firing(void)             { return pyro_is_firing(); }
bool hal_pyro_fault(uint8_t channel)      { return pyro_fault(channel); }
```

So a new `src/pyro_mk1c.c` implementing the six functions in `src/pyro.h` requires
**zero changes to `hal_hardware.c`'s pyro section**.

`hal_hardware.c` is ~800 lines, and apart from the pyro delegation it is the UART ISR
ring buffer, littlefs, config persistence, the async task runner, flight logging, and
platform init — all board-independent. Its only board-specific content is:

| Line | Content | MK1B | MK1C |
|---|---|---|---|
| 38, 328-337 | `BUZZER_PIN` | 16 | 11 |
| 156, 586-588 | heartbeat LED | 25 | 8 |
| 596-597 | UART0 TX/RX | 0, 1 | 0, 1 (same) |
| 600-601 | `adc_gpio_init` | 26, 27 | 26, 27, 28, 29 |

**Board-specific leaf files:**

| File | Disposition |
|---|---|
| `src/pyro.c` | MK1B only. Untouched. |
| `src/pressure_sensor.c` | MK1B only (dual-SDA BMP280/MS5607 probe). Untouched. |
| `src/bmp280_driver.c` | MK1B only. Untouched. |
| `src/ms5607_driver.c` | **Shared unchanged** — `I2C_PORT` is `i2c1` on both boards |
| `src/hal_hardware.c` | Shared, with the four differences above |

**Flight-logic consumers of the pyro HAL** (board-independent, but they encode MK1B's
electrical model): `flight_states.c:93-163` (`try_fire_pyros`, `check_pyro_fault`,
`check_post_fire_verify`, `check_refire`), `flight_states.c:181,209`
(`detect_boot_continuity`, `update_continuity_and_buzzer`), `ground_test.c:105,121`
(`ARM`/`FIRE` serial commands).

### Key Discoveries

Confirmed by exporting the netlist with
`kicad-cli sch export netlist --format kicadsexpr` from
`~/Documents/pyro_mk1c/pyro_mk1c.kicad_sch`, not by reading the schematic images.

**MK1C pin map (U3 = bare RP2040, QFN-56):**

| GPIO | Net | GPIO | Net |
|---|---|---|---|
| 0 | `TX0/SDA0` → J1.4 | 17 | `FIRE_A` |
| 1 | `RX0/SCL0` → J1.5 | 18–21 | J3 (connector not fitted — X'd out) |
| 2–5 | unconnected | 22 | `GPIO` → J1.6 (spare) |
| 6 | `SDA1` (i2c1) | 23 | `BIAS_BUS` |
| 7 | `SCL1` (i2c1) | 24 | `FIRE_B` |
| 8 | `LED` → R4 1 kΩ → D1 (blue) | 25 | `BIAS_B` |
| 9, 10 | unconnected | 26 / ADC0 | `SNS_VBAT` |
| 11 | `buzzer` → Q2 AO3400A gate | 27 / ADC1 | `SNS_BUS` |
| 12 | `ARM_TOGGLE` → C101 10 nF pump | 28 / ADC2 | `SNS_A` |
| 13–15 | unconnected | 29 / ADC3 | `SNS_B` |
| 16 | `BIAS_A` | RUN | SW2 reset button; SW1 = nBOOTSEL |

- **ILM and FLT are not routed to the MCU.** `U9` pin 6 (`~FLT`) goes only to a pull-up;
  pin 7 (`ILM`) goes only to R130 499 Ω → GND. This is how the board closed `DESIGN.md`
  §12's ADC budget — it kept SNS_VBAT and dropped ILM. Confirmed final by the user.
- **`DESIGN.md` §S10 is stale**: it puts the D1 heartbeat on GPIO25. GPIO25 is `BIAS_B`
  on this board. D1 is on GPIO8.
- **One pressure sensor.** i2c1 carries only U2 `MS560702BA03-50`, `PS` tied to +3.3 V
  (I2C mode), `CSB` tied to GND. No BMP280 and no IMU are fitted, despite both symbols
  existing in the schematic library cache.
- **`board_init()` claims GPIO25.** `$PICO_SDK_PATH/lib/tinyusb/hw/bsp/rp2040/family.c:152-155`
  does `gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT)` whenever
  `PICO_DEFAULT_LED_PIN` is defined, and `boards/pico.h:35` defines it as 25. On MK1C
  that drives `BIAS_B` as an output. The MK1C board header must **not** define
  `PICO_DEFAULT_LED_PIN`. (`hal_hardware.c:583` already carries a comment noting that
  `board_init()` reinitialises this pin — the interaction is live today, just harmless
  on MK1B.)
- **16 MB flash.** U4 is `XT25F128FWOIGT-W` (128 Mbit). `CMakeLists.txt:26` sets
  `PICO_BOARD pico` (2 MB) and line 58 sets `PFB_RESERVED_FILESYSTEM_SIZE_KB 984`, both
  sized for a Pico.
- **RP2040 pads reset to input-with-pull-down**, so `FIRE_A`, `FIRE_B`, `BIAS_*` and
  `ARM_TOGGLE` are all inherently safe at power-on: the board comes up in S0 by
  construction, before any code runs. This is worth preserving and asserting, not
  reinventing.
- **ADC scale.** All four dividers are 0.3329 (4.99/14.99 and 49.9/149.9). At 12 bits
  and a 3.3 V reference this gives **2421 µV of node voltage per count**, i.e.
  **413 counts per volt at the node** — which reproduces every number in `DESIGN.md` §4
  (2.56 V → 1058 counts; 8.4 V bus → 3469 counts). This constant is the single most
  error-prone value in the whole port; derive it once and test it.
- **The fire trigger is ratiometric** (SNS_BUS ≥ 0.90 × SNS_VBAT, both through the same
  ADC), so RP2040 INL/DNL largely cancels. The tracking test's ~10:1 margin
  (1030 counts vs <50) is far outside any RP2040 ADC error.
- **`check_refire()`** (`flight_states.c:145-163`, PYR-REFIRE-01) re-fires a channel
  1–1.5 s after the first attempt if still ballistic. This is in tension with
  `DESIGN.md` invariant 4 ("latch all faults, never retry automatically") — see Open
  Questions.
- **MK1B firmware on MK1C hardware** leaves GPIO17 (`FIRE_A`) as an input with a pull-up
  (`pyro.c:39-41`), which turns Q403's gate on. It cannot actually fire, because MK1B
  never toggles GPIO12 and the firing bus stays cold — but this must be blocked by a
  build/artifact guard rather than left to that coincidence.
- **R_BLEED is a single 2.2 kΩ resistor (R103), not the split pair `DESIGN.md` §3
  specifies.** The netlist shows only `R103 2.2kΩ` from `FIRING_BUS` to GND. §3 calls for
  "2 × 4.7 kΩ in parallel … split so that one open resistor cannot defeat it", and §10
  names an open bleed as the single-point fault that strands the bus charged. As
  fabricated, that mitigation is absent. §S8's 25 s electrical worst case assumed *one of
  two* bleed resistors open; with one resistor there is no partial-failure case, only
  total loss of the only discharge path. **Raise with the hardware owner.**
- **The §S9 indicator network is not in the netlist.** No `MMBT3904`, no R129/R7/D7. The
  bus node therefore carries no 16.7 kΩ indicator load. This cancels out: bus pull-down
  is `2.2k ∥ 14.99k = 1.918 kΩ`, against §4's 1.92 kΩ for a different resistor set, so
  every bias level in §4 still holds. But the armed-and-active lamp that §S8's approach
  rule depends on — "look at the indicator; a lit lamp means do not approach" — does not
  exist on this board. **Raise with the hardware owner.**
- **`SPECIFICATION.md:50-64` is already wrong for MK1B** (it lists I2C on GPIO 8/9 and a
  GPIO 8 test jumper; the code uses 6/7/10 and there is no jumper). Fix it in the same
  pass rather than adding a second stale table beside it.

## The HAL contract, unchanged

Every MK1C behaviour maps onto the existing six `pyro.h` functions. This table is the
design contract for `pyro_mk1c.c` and should be reproduced as a comment at the top of
that file.

| `pyro.h` function | MK1C implementation |
|---|---|
| `pyro_init()` | Drive `FIRE_A/B`, `BIAS_A/B/BUS`, `ARM_TOGGLE` low as outputs; init ADC0–3. Enter S0. Asserts, rather than establishes, the safe pad-reset state. |
| `pyro_check_continuity(p1, p2)` | The §S3 tracking test: bias the bus only, sample all three nodes, release. Returns the latest duty-cycled result rather than blocking. |
| `pyro_fire(channel)` | Runs F0–F8 as a bounded blocking call (~50 ms worst case); F9/F10 continue asynchronously in `pyro_update()`. |
| `pyro_update(now_ms)` | Drives the duty-cycled tracking test, the F9 drain watch, the F10 verify, and the S0–S7 electrical state machine. Already called every main-loop iteration (`flight_states.c:767`). |
| `pyro_is_firing()` | True from F0 until F10 completes. |
| `pyro_fault(channel)` | No FLT pin exists. Returns the **latched** fault state for that channel — the F5 misfire classification plus any §8.1 diagnostic latch. This preserves the `hal.h:47` semantic ("true = fault during fire") and keeps `EVT_PYRO*_FAULT` meaningful. |

`pyro_continuity_t` (`pyro.h:7-12`) carries `raw_adc`, `good`, `open`, `shorted` — enough
for the tracking test without modification:

- `raw_adc` = that channel's raw tracking count. §S3 requires logging the raw value, not
  a boolean, because a dirty 5 kΩ connector reads ~190 counts and passes a naive
  threshold. This field already reaches telemetry via `ctx->pyro1_adc`.
- `open` = count < 400 (the §S3 threshold)
- `good` = count ≥ 400 — match present
- `shorted` = this channel has a latched short-type diagnostic (shorted low-side FET,
  drain-to-ground, shorted TVS). The routine tracking test biases only the bus, so it
  cannot itself distinguish shorted from present; those diagnoses come from the §8.1
  classifier and latch.

**Things that deliberately do *not* enter the HAL**, because they are policy rather than
hardware and the flight layer already has the state they need:

- **Authority** (invariant 11). The two authorities are the flight sequencer and the
  ground-test harness — which is exactly the distinction between the `flight_states.c`
  and `ground_test.c` call sites. `ground_test.c` already refuses commands outside
  PAD_IDLE (GND-TEST-04). No parameter needs to cross the HAL boundary.
- **S8 approach clearance and the context branch** (invariant 12). Locking out fire
  commands for 60 s is a policy decision that needs the flight state; `flight_states.c`
  has it and `pyro_mk1c.c` does not.
- **Buzzer annunciation patterns** (§S8). `buzzer.c` already sits above `hal_buzzer_*`.
- **Fault reset.** Invariant 4 requires an explicit operator reset, and invariant 6
  requires entering S0 on reset/brownout/watchdog. The RUN button (SW2) and a power
  cycle both satisfy this, so no clear-fault entry point is needed.

### S1 and S1B are removed

**S1 (self-test) and S1B (hardware test) are not implemented, in this pass or any
later one.** Both assert `FIRE_x` or run `ARM_TOGGLE` outside a real fire command — S1
step 5 asserts `FIRE_x` to prove the low side closes, S1 step 6 runs the pump to prove
U9 turns on, and S1B delivers a real pulse into a dummy load. The board must not contain
code that energises the firing path for any reason other than firing.

This becomes an invariant, and it is stronger than anything in `DESIGN.md` §9:

> **Invariant 13a. `FIRE_A` and `FIRE_B` are asserted only by the F0–F10 firing sequence,
> on a real fire command from a valid authority. No diagnostic, test, self-check, boot
> path or operator command asserts either signal, ever.**
>
> **Invariant 13b. `ARM_TOGGLE` is asserted only by the F0–F10 firing sequence or by the
> optional bus precharge test (T4, Phase 4.3), the latter behind its full interlock.**

The asymmetry is deliberate and follows the circuit. Current reaches a bridgewire only
when the bus is energised **and** the low-side FET conducts **and** the mechanical
disconnect is closed — `DESIGN.md` §2, "when the two switches are open, the two ends of
the match float". Energising the bus alone completes no circuit. Closing the low-side
switch is the step that does, so that is the one no test may take.

The payoff is that every inadvertent-fire path collapses into a single function, which can
be reviewed as a unit. It is also directly checkable — see the Phase 7 grep gate.

Everything the diagnostics need is reachable **bus-cold**, using only the three
current-limited bias injectors and the four ADCs. See Phase 3.

## Desired End State

`cmake -DPYRO_BOARD=mk1b` reproduces today's firmware bit-for-behaviour.
`cmake -DPYRO_BOARD=mk1c` produces firmware that boots on MK1C hardware, enumerates over
USB, streams telemetry on J1, samples the MS5607, and implements S0–S7 and F0–F10 behind
the unmodified `pyro.h`, with S8 and the context branch in the flight layer.

**Verification**: `cmake --build build --target host_tests integration_tests closedloop_tests`
passes for both board settings; both `.uf2` artifacts build; the Phase 8 bench checklist
is signed off.

## What We're NOT Doing

- **Not changing `src/hal.h` or `src/pyro.h`.** If a phase appears to need a new HAL
  function, that is a signal the behaviour belongs in the flight layer — revisit the
  design before widening the contract.
- **Not touching `src/pyro.c`, `src/pressure_sensor.c` or `src/bmp280_driver.c`.** They
  are the MK1B implementation and are simply not compiled for MK1C.
- Not introducing a `src/boards/` pin-abstraction layer. Each board's leaf files own
  their own pins; `hal_hardware.c` needs four small `#if`s and nothing more.
- **Not implementing S1 or S1B, ever.** Both energise the firing path outside a real
  fire. Their detection coverage is replaced by the bus-cold set in Phase 3, except for
  the three residuals listed there.
- Not respinning the board to route ILM or FLT. The user confirmed the current routing
  is final; the firmware adapts.
- Not implementing `DESIGN.md` §7.2's capacitance-from-ILM inference, or §12's 2:1 mux.
- Not supporting the BMP280 or the KXG0903 IMU on MK1C — neither is fitted.
- Not modelling the MK1C pyro chain in the WASM sim (`sim/hal_sim.c` is untouched; it
  already satisfies `hal.h`).
- Not adding runtime board detection. There is no ID pin.
- Not touching the lwIP/TinyUSB networking stack, the OTA bootloader internals, or the
  web UI beyond the board-name string and the OTA board guard.

## Implementation Approach

Phase 1 makes the build select a board. Phase 2 gets MK1C booting with a stub pyro
backend. Phases 3–5 build `pyro_mk1c.c` bottom-up — sense, then arm, then fire — so that
nothing energises the firing bus until Phase 4 and nothing fires until Phase 5. Phase 6
adds the flight-layer policy. Phases 7–8 are guards, docs and bench sign-off.

---

## Phase 1: Build-system board selection

### Overview

Teach CMake which board it is building. No source file changes beyond a name macro.

### Changes Required

#### 1.1 `CMakeLists.txt`

- `set(PYRO_BOARD mk1b CACHE STRING "Target board: mk1b or mk1c")` — **default stays
  mk1b**, so an unqualified build keeps producing what it produces today.
- Validate against the allowed set; `message(FATAL_ERROR)` otherwise.
- `target_compile_definitions(pyro_fw_c PRIVATE PYRO_BOARD_MK1B=1 PYRO_BOARD_NAME="Pyro MK1B" PYRO_BOARD_SHORT="mk1b")` (or the MK1C equivalents).
- Select the board's leaf sources. The `add_executable` list at `CMakeLists.txt:93-110`
  gains a variable in place of the three board-specific entries:

  | | mk1b | mk1c |
  |---|---|---|
  | pyro backend | `src/pyro.c` | `src/pyro_mk1c.c` |
  | pressure probe | `src/pressure_sensor.c` | `src/pressure_sensor_mk1c.c` |
  | BMP280 driver | `src/bmp280_driver.c` | *(omitted)* |

  `src/ms5607_driver.c` stays in the common list — `I2C_PORT` is `i2c1` on both boards.
- `pico_set_program_name(pyro_fw_c ${PYRO_BOARD_NAME})` — replaces the literal at line 165.
- `set_target_properties(pyro_fw_c PROPERTIES OUTPUT_NAME "pyro_fw_${PYRO_BOARD}")` so the
  artifacts are distinguishable and an MK1B image is not casually flashed to MK1C.
- Pass `-DPYRO_BOARD_*` to the three host-test custom targets (lines ~209, ~232, ~255).

#### 1.2 Board-name strings in shared files

`usb_descriptors.c:89`, `http_server.c:248` and `flight_states.c:656` are compiled for
both boards and contain the literal `"Pyro MK1B"`. Replace with `PYRO_BOARD_NAME`.

`hal_hardware.c:703` also has the literal, but see Phase 2.2 — it stays shared, so it
gets the macro too.

### Success Criteria

#### Automated
- [ ] `cmake -B build -DPYRO_BOARD=mk1b && cmake --build build` succeeds
- [ ] `cmake --build build --target host_tests integration_tests closedloop_tests` passes
- [ ] `cmake -B build-x -DPYRO_BOARD=bogus` fails with a clear message
- [ ] `git diff --stat src/pyro.c src/pressure_sensor.c src/bmp280_driver.c` is empty

#### Manual
- [ ] An MK1B board flashed with the Phase 1 build behaves identically

---

## Phase 2: MK1C platform bring-up (everything except pyro)

### Overview

Make MK1C boot, enumerate, talk and sense pressure, with a stub pyro backend that holds
every output low.

### Changes Required

#### 2.1 Custom Pico SDK board header

New `src/boards/sdk/pyro_mk1c.h`, referenced via `PICO_BOARD=pyro_mk1c` and
`PICO_BOARD_HEADER_DIRS`. (This is an SDK board header, which the SDK requires by this
name and location — it is not the pin-abstraction layer this plan avoids.) It must:

- **Not define `PICO_DEFAULT_LED_PIN`** — see *Key Discoveries*. This is the entire
  reason a custom header is needed rather than `PICO_BOARD=pico`.
- `#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)`
- Set `PICO_DEFAULT_UART 0`, TX 0, RX 1
- Set `PICO_DEFAULT_I2C 1`, SDA 6, SCL 7 — and *not* inherit `pico.h`'s SDA 4 / SCL 5,
  which are NC here
- Confirm the XIP/QSPI defaults suit the XT25F128 (the 12 MHz crystal is standard)

#### 2.2 `hal_hardware.c` — four `#if`s

This file stays shared. Copying ~700 lines of UART ISR, littlefs, config and task-runner
code into a second HAL implementation would mean fixing every future bug twice, in code
that has already needed careful UART and networking fixes (commits 1116373, f9e66b6).
The board-specific content is four items:

| Site | Change |
|---|---|
| `:38` | `BUZZER_PIN` 16 → 11 under `#if PYRO_BOARD_MK1C` |
| `:156` | `gpio_xor_mask(1u << 25)` → `1u << PYRO_LED_PIN` |
| `:586-588` | `gpio_init(25)` / `set_dir` / `put` → `PYRO_LED_PIN` |
| `:600-601` | `adc_gpio_init(26/27)` → also 28, 29 on MK1C |

`:596-597` (UART pins 0 and 1) needs no change — both boards use the same pins.

#### 2.3 New `src/pressure_sensor_mk1c.c`

Implements `pressure_sensor.h` for a single MS5607 on i2c1 (GPIO 6/7):

- Configure i2c1 once; no SDA pin switching, no `release_i2c_pin()`, no BMP280 probe
- **Keep the 9-pulse bus-recovery preamble.** It exists because the sensor survives a CPU
  reset mid-transaction (commit 5019cf2), which is just as true on MK1C
- Probe MS5607 only. `CSB` is tied low; the existing driver already sweeps 0x76–0x77
  (`ms5607_driver.c:83`), so leave that sweep alone rather than hardcoding an address

#### 2.4 Stub `src/pyro_mk1c.c`

`pyro_init()` drives `FIRE_A`, `FIRE_B`, `BIAS_A`, `BIAS_B`, `BIAS_BUS`, `ARM_TOGGLE`
low as outputs and initialises ADC0–3. The other five functions report nothing happening
and change no pin state. This satisfies `pyro.h` so the board links and boots.

#### 2.5 OTA / filesystem layout for 16 MB

`PFB_RESERVED_FILESYSTEM_SIZE_KB` (`CMakeLists.txt:58`) is 984, sized for 2 MB. Set it
per board, recompute the A/B slots and the littlefs region for 16 MB, and add a compile-time
assertion against `PICO_FLASH_SIZE_BYTES` so a wrong number fails the build rather than
corrupting flash.

### Success Criteria

#### Automated
- [ ] `cmake -B build-mk1c -DPYRO_BOARD=mk1c && cmake --build build-mk1c` succeeds
- [ ] A static assertion ties `PFB_RESERVED_FILESYSTEM_SIZE_KB` to `PICO_FLASH_SIZE_BYTES`

#### Manual
- [ ] Board enumerates over USB; the web UI loads
- [ ] D1 (GPIO8) blinks from the main loop
- [ ] `$PYRO` telemetry appears on J1.4 at 115200
- [ ] MS5607 detected; altitude tracks a squeeze test
- [ ] **Scope GPIO 12, 16, 17, 23, 24, 25 from power-on through boot and confirm all six
      stay low.** Validates that `board_init()` no longer claims GPIO25 and that the pad
      reset state survives
- [ ] littlefs mounts and survives a power cycle; OTA upload succeeds

---

## Phase 3: `pyro_mk1c.c` — sense layer and the tracking test

### Overview

Implement `pyro_check_continuity()` properly. The bus is never energised in this phase,
so it is safe to bench with a real e-match fitted (tracking current is ~0.2 mA against a
≥100 mA no-fire current).

### Changes Required

#### 3.1 ADC scaling

One helper using 2421 µV/count. Integer-only, per DECISIONS.md #1. Round-trip tested
against the four anchor values from `DESIGN.md` §4 (2.56 V/1058, 2.94 V/1214,
2.51 V/1037, 8.4 V/3469).

#### 3.2 Sampling

Median of 3 per §7.1 step 3. A 4-channel round-robin snapshot (`SNS_VBAT`, `SNS_BUS`,
`SNS_A`, `SNS_B`) for the slow paths; a single-channel fast path for the Phase 4 ramp.

#### 3.3 Tracking test (§S3)

- Assert `BIAS_BUS` only; `BIAS_A` / `BIAS_B` stay low
- 5–10 ms of bias, then sample all three nodes, then release
- Duty cycle: one test per few hundred ms, driven from `pyro_update()`

`pyro_check_continuity()` returns the **latest cached result**, so it stays non-blocking
for its callers at `flight_states.c:128,137,151,160,181,209`. This is a behaviour change
from MK1B's blocking 10 ms check, and a safe one — every caller already tolerates a
slightly stale reading, because `update_continuity_and_buzzer()` only polls at 1 Hz.

#### 3.4 Bounds checking, raw logging, and the fault latch

**The firmware does not classify faults.** It compares each measurement against its
expected band, latches on a reading outside that band, and logs the raw count. The table
below is the *documented interpretation* of an out-of-band reading — reference material
for bench work and for the fault codes' meaning, not a decision tree the firmware walks.

Two of these rows are safety decisions rather than diagnoses, and those the firmware does
act on directly: a bus at pack voltage with the pump stopped, and a channel node at ~0
under its own bias. Both mean **do not arm**, and both latch.

| Snapshot | Interpretation |
|---|---|

| Snapshot | Diagnosis |
|---|---|
| `SNS_BUS` ≈ 3469 counts, pump stopped | high side shorted to battery → latch, do not arm |
| `SNS_BUS` ≈ 0, no ramp under bias | bus shorted to ground → latch |
| bus ≈ 1058 counts under bias | bus healthy |
| channel ≈ 0 under its own bias | low-side FET shorted, or drain shorted to ground → latch |
| channel ≈ 1037 where 1214 expected | **TVS shorted** → phantom continuity; latch, distrust this channel's presence |
| channel high, bus cold, no bias | leakage, or the mechanical arm is closed when it should be open |
| channel follows bus bias (~1030) | match present |
| channel ≈ 0 during bus bias | match open or absent |

Invariants 8 and 9 apply: N consecutive agreeing samples before latching; outlier
rejection never applies to *clearing* a fault. The latch feeds
`pyro_continuity_t.shorted` and `pyro_fault()`, and `raw_adc` carries the raw count to
telemetry and the flight log in every case.

#### 3.5 The bus-cold diagnostic set

With S1 removed, these three stimuli are the **entire** diagnostic capability, and all of
them run with U9 off, the bus at 0 V, and `FIRE_A`/`FIRE_B`/`ARM_TOGGLE` never asserted
(invariant 13). Each is driven from `pyro_update()` and surfaces through
`pyro_check_continuity()` and `pyro_fault()`.

**T1 — quiescent read. No stimulus at all.** All bias off. Sample all four ADCs. Free, so
run it every cycle in every state, including flight.

| Reading | Diagnosis |
|---|---|
| `SNS_BUS` ≈ `SNS_VBAT` (3469 counts at 8.4 V) | **high side shorted** — U9 failed short. Do not arm. Latch. |
| `SNS_BUS` ≈ 0 | bus cold, normal |
| `SNS_BUS` mid-band | U9 leakage, or a partial short |
| `SNS_A`/`SNS_B` > ~50 counts, bus cold | channel leakage, or the mechanical arm is closed when it should be open |

**T2 — bus bias.** `BIAS_BUS` on for 5–10 ms, duty-cycled once per few hundred ms. This is
the §S3 tracking test. ~0.2 mA through the bridgewire, 500× below no-fire.

| Reading | Diagnosis |
|---|---|
| bus → 1058 ± band | bus healthy **and R_BLEED present** |
| bus → ~1214 | **R_BLEED open** — see below |
| bus → < 50, no ramp | **bus shorted to ground**, or a fitted match is shorting to ground through a channel |
| channel follows bus (~1030) | **match present** |
| channel < 50 | **match open or absent** |
| channel ~190 | degraded match or dirty connector at ~5 kΩ — log the raw count |

T2 measures the bus pull-down resistance against a known 330 Ω source, so it reads
R_BLEED directly. On this board R103 is the only bleed, so losing it moves the bus node
from 1058 to 1214 counts — **+156 counts, ~15%**, an unambiguous signature. (Had the
§3 split pair been fitted, one open resistor would shift it only +57 counts, inside
tolerance stack-up. The single-resistor build is worse for redundancy but easier to
diagnose.)

**T3 — per-channel bias.** `BIAS_A` alone, then `BIAS_B` alone, bus cold. ~1.4 mA, 70×
below no-fire. This is `DESIGN.md` §S1 *passive* step 3 — it asserts no `FIRE_x` and runs
no pump, so it survives invariant 13.

| Reading | Diagnosis |
|---|---|
| node → 1214 | channel node healthy and isolated |
| node → ~1037 | **TVS shorted** — phantom continuity; distrust this channel's presence |
| node → ~0 | **low-side FET stuck in the fire state** (drain-source short), or drain shorted to ground |
| node → mid-band (e.g. ~288 counts) | **partially conducting FET** — ~100 Ω drain-source. Log the raw count; do not round to a boolean |

T3 is load-bearing and cannot be dropped: a shorted low-side FET with a match fitted also
drags the bus down in T2, so T2 alone cannot tell it from a bus-side short — and with the
match *disconnected*, T2 cannot see it at all.

**Discrimination.** The three stimuli together resolve every short-high / short-low /
open case. This table is reference material for bench work — at the pad, any row other
than the first is simply NO-GO:

| T1 bus | T2 bus | T3 A | T3 B | Diagnosis |
|---|---|---|---|---|
| ~0 | 1058 | 1214 | 1214 | all healthy |
| **≈ VBAT** | — | — | — | **high-side short** (T1 alone; do not proceed to T2) |
| ~0 | 1058 | 1214 | **<50** | **low-side short, channel B** |
| ~0 | **<50** | 1214 | 1214 | **bus shorted to ground** |
| ~0 | **<50** | **<50** | 1214 | **low-side short, channel A**, dragging the bus via a fitted match |
| ~0 | **1214** | 1214 | 1214 | **R_BLEED open** |
| ~0 | 1058 | ~1037 | 1214 | **TVS shorted, channel A** |
| ~0 | 1058 | 1214 | 1214, **ch <50 in T2** | **match open or absent** |

Invariants 8 and 9 apply throughout: N consecutive agreeing samples before latching;
outlier rejection never applies to *clearing* a fault.

#### 3.6 What the bus-cold set cannot see

Three residuals, all of which were S1/S1B's unique coverage. Each needs an explicit
owner, because none of them is a firmware problem any more.

**Note the asymmetry in the low-side failure modes.** A FET stuck in the *fire* state is a
drain-to-source short — the safety-critical mode, and the one `DESIGN.md` §2 names as the
important one — and T3 catches it directly (node → ~0), with T2 corroborating whenever a
match is fitted. Only the *non-conducting* mode is invisible bus-cold, and that is a
mission-reliability fault rather than a safety one.

| Undetected | Why bus-cold cannot see it | Where it surfaces instead |
|---|---|---|
| **Low-side path fails to conduct when commanded** — the FET die, the 100 Ω gate resistor, the 1 MΩ pull-down or the GPIO path being open. *Not* the stuck-on case, which T3 catches | A switch can only be proven to close by closing it | F5 of a real fire: the bus fails to collapse when `FIRE_x` asserts. Pre-fire T2 already confirmed the match was present, so this is distinguishable from an open match — but only after the fact. **Misfire risk, not an inadvertent-fire risk** |
| **U9 turn-on, dV/dt ramp, pump polarity** | Requires arming | **T4, the optional bus precharge test (Phase 4.3)** — or, if T4 is not run, F2 of the first real fire, with the timeout abort |
| **Match shorted at 0 Ω** (pinched lead, spent match) | Already not detectable in-system — `DESIGN.md` §8.2 says so | Pre-flight **visual inspection**. Unchanged by this decision |

The end-to-end firing-path test (§S1B) has no substitute and is simply gone.

### Success Criteria

#### Automated
- [ ] Host tests for the ADC conversion against the four §4 anchors
- [ ] Host tests for the §8.1 classifier covering every row above
- [ ] Host test: a single outlier never latches; N agreeing samples do
- [ ] Host test: a single hot-bus sample **does** block a clearance (invariant 9)

#### Manual
- [ ] Bus bias, no match: `SNS_BUS` reads 1058 ± tolerance
- [ ] Bus bias, one match: bus and that channel both ~1040
- [ ] Open channel reads < 50 counts while the other reads ~1030
- [ ] Measured bias current through a match ≈ 0.2 mA
- [ ] `SNS_VBAT` tracks a bench supply across the 1S and 2S range

---

## Phase 4: `pyro_mk1c.c` — ARM_TOGGLE pump and precharge

### Overview

The safety-critical core. **Invariant 5**: the toggle is generated in software, in the
code path that has just re-checked every arm condition. Never from a timer or PWM.

### Changes Required

#### 4.1 The toggle primitive

A bounded, blocking pump loop that re-evaluates arm conditions every cycle and returns
as soon as any fails — which stops the pump and disarms by construction.

- 10–50 kHz square wave on `ARM_TOGGLE` (100 µs period at the bottom of the range;
  §5.1 gives ~10× margin there)
- Each cycle re-checks: arm conditions still hold, no latched fault, `SNS_VBAT` above the
  firmware UVLO, the commanded sequence not aborted
- A `for` loop with an explicit iteration bound — no `while (1)`, no peripheral

**Do not add a PWM slice, a repeating timer, a DMA pacer, or a PIO program for this
signal.** A comment at the definition should say why, citing invariant 5, because it
looks like an obvious optimisation to a later reader.

#### 4.2 Precharge (F1/F2)

- Start the pump; ~0.4 ms to the U9 enable threshold
- Sample `SNS_BUS` every 50 µs, median of 3
- Trigger condition: `SNS_BUS ≥ 0.90 × SNS_VBAT`, with `SNS_VBAT` **measured at F0**,
  not a constant (§7.2)
- Abort on timeout = 1.5 × expected time for the build variant (≈ 8.5 ms on 2S,
  ≈ 4.2 ms on 1S at 0.89 V/ms)

**ILM substitute.** §7.2's capacitance check and the F2 "ILM disagrees" abort have no
hardware source. The precharge timeout is the only remaining guard against a
wrong-capacitance build. This also removes the cross-check that §S8 credits §7.2 with —
"§7.2 measures the capacitance on every precharge and §8.2 latches on a mismatch, thus an
out-of-spec capacitor cannot silently invalidate the number" is no longer true. The
mitigation is that the ALL CLEAR placard is already sized on the largest capacitance the
pads accept. Record both points in `DESIGN.md` (Phase 7.3).

#### 4.3 T4 — optional bus precharge test

Operator-commanded, never automatic. It runs the pump and charges the bus with the gates
held low; `FIRE_A` and `FIRE_B` are never asserted (invariant 13a).

**Interlock — all conditions required:**

| Condition | Rationale |
|---|---|
| Explicit operator command | Never on boot, after a reset, or after S2 |
| Ground context | |
| T2 reads **open** on both channels | No live match is in circuit |
| T3 reads **~1214** on both channels | Proves the bias injectors are intact, so T2's "open" can be trusted — **and** proves neither low-side FET is shorted |
| No latched faults; `SNS_VBAT` above UVLO | |
| Mechanical disconnect open | **Operator confirms by eye. Firmware cannot verify this** (`DESIGN.md` §S8) |

The T3 condition is what closes the false-negative hole. An open bias injector (D104 or
R112) would make a live, firable channel read *open* in T2; requiring T3 to read 1214
proves the injection path works, so a T2 "open" means the circuit really is broken. Note
that T3 ≈ 0 aliases three causes — shorted FET, drain-to-ground short, or open injector —
and all three must block T4, so the ambiguity is safe even though it is diagnostically
unhelpful.

**Sequence:** gates low → start the pump → sample `SNS_BUS` every 50 µs → stop the pump →
watch the decay. Require `SNS_BUS` below the indicator threshold before permitting
anything else.

**What T4 uniquely verifies:**

| Check | Why it matters |
|---|---|
| The pump works and D2 is oriented correctly | §5.1 records that draft 0.2 drew D2 backwards *and the first schematic built from it repeated the error*. A reversed D2 means the board can never arm |
| U9 turns on; ramp ≈ 0.89 V/ms | |
| **The `SNS_BUS` ≥ 0.90 × `SNS_VBAT` comparison itself** | This is the F3 fire trigger. Without T4 it is first exercised at apogee. A swapped ADC channel or a wrong divider would not be found until then |
| Passive disarm: stop the toggle → the bus stops rising within ~9.6 ms | The only disarm path there is |
| **Decay time constant → fitted capacitance** | See below |

**The decay recovers the capacitance check that losing ILM took away.** The bus pull-down
is a known 1.918 kΩ, so τ = 1.918 kΩ × C_BULK. That gives 0.19 s, 1.92 s and 4.22 s at
100 µF, 1000 µF and 2200 µF — an order of magnitude apart and trivially separable. §7.2
wanted this from ILM during the *ramp*; T4 gets it from the *decay* instead. It is slower
and it needs an operator command, but it is the same information, and it gives the
build-variant setting (Open Question 5) the cross-check it otherwise lacks.

#### 4.4 The pad test

> **Scope. The only question the pad test answers is whether the vehicle is safe to fly.
> Diagnostics are out of scope.** Every measurement is a bounds check against an expected
> band. Outside the band is NO-GO, and the beep code names the **step that failed**, not a
> root cause. Working out *why* is a bench activity, done later from the logged raw counts.

This has three consequences for the implementation:

- **No decision tree.** One comparison per measurement, one latch, one code. The firmware
  never branches on which fault a reading implies.
- **Bands, not thresholds.** A reading is checked against a window around its expected
  value, not a single-sided threshold. A dirty connector at ~5 kΩ reads ~190 counts and a
  partially conducting FET reads ~288; both are outside their band and both are NO-GO.
  There is no "degraded — investigate" state at the pad, because investigating is not
  something that happens at the pad.
- **Log raw counts, always.** Every step records its raw ADC value regardless of pass or
  fail. That is what makes bench diagnosis possible afterwards, and it is the same rule
  §S3 already states for the tracking test.

The operator-facing procedure ties T2, T3 and T4 together. "Interlock" here is the 2-pole
mechanical disconnect: **asserted** = open = match leads broken; **removed** = closed =
match in circuit.

The right-hand column is **bench interpretation only** — it is not logic the firmware runs
and not information the pad test reports. At the pad, "outside band" is the whole answer.

| # | Interlock | Stimulus | Expect (GO band) | Bench interpretation of a NO-GO |
|---|---|---|---|---|
| 1 | — | operator asserts interlock | — | |
| 2 | in | `BIAS_A` | **1214** | 1037 = shorted TVS · ~0 = shorted FET, drain-to-ground, or open injector · anything else = interlock not actually asserted |
| 3 | in | `BIAS_B` | **1214** | as above |
| 4 | in | `ARM_TOGGLE`, gates low | ramp 0.89 V/ms to ≥ 0.90 × `SNS_VBAT`; `SNS_A`/`SNS_B` stay < 50 | no ramp = pump, D2 orientation or U9 fault · timeout = C_BULK too large or bus loaded · **either channel rising → abort immediately** |
| 5 | in | stop the pump | decay with τ = 1.918 kΩ × C_BULK | no decay = R_BLEED open or high side shorted |
| 6 | in | none | `SNS_BUS` < 50 | gate before the operator touches the disconnect |
| 7 | — | operator removes interlock | — | |
| 8 | out | `BIAS_A` | **1037** | 1214 = match absent or open · ~0 = short to ground · mid-band = degraded match or dirty connector |
| 9 | out | none | `SNS_BUS` < 50 | |
| 10 | out | `BIAS_B` | **1037** | as step 8 |
| 11 | out | none | `SNS_BUS` < 50 | |
| 12 | out | — | success beep, READY | any failure → fail beep + step code |

**Why the ordering matters, and must not be changed.** Under per-channel bias a connected
match and a shorted TVS read *identically* — both tie the node to the bus pull-down, both
give 2.51 V / 1037 counts (`DESIGN.md` §4 lists the shorted-TVS case at exactly this
value). Steps 2 and 3 resolve the ambiguity by measuring with the match **isolated**,
where healthy reads 1214 and only a shorted TVS reads 1037. Having cleared the TVS, a 1037
at steps 8 and 10 unambiguously means the match is present. Reordering the procedure
destroys this.

**The pad test verifies the interlock itself.** Firmware cannot read the disconnect
position statically — `DESIGN.md` §S8 is right that a safed channel is indistinguishable
from no match fitted. But the *transition* across step 7 is observable: 1214 → 1037 on
both channels proves the disconnect moved and is working. This is new capability the
documents do not currently claim, and it should be written into §S8 alongside the existing
(and still correct) warning about the safing direction.

**Steps 2 and 3 precede step 4 deliberately** — this reorders the originally specified
sequence, which charged the bus first. The physical barrier does the real work —
with the match isolated, energising the bus can deliver current nowhere. But if the
operator has *not* asserted the interlock and a match is connected, a single shorted FET
turns step 4 into a live fire. Steps 2 and 3 are bus-cold and free, and they catch that
case before anything is energised. The `SNS_A`/`SNS_B` monitor during the step 4 ramp is
the backstop: at 0.89 V/ms the sequence can abort below 1 V, about 1.1 ms in, if either
channel node starts to follow the bus.

**Steps 8 and 10 charge the bus as a side effect**, which is why steps 9 and 11 exist.
With the match connected, channel bias couples through the bridgewire to the bus, so both
nodes sit at ~1037 and the bus capacitance charges to 2.51 V. Draining between channels is
required for a valid measurement — residual charge would make an *absent* match on the
second channel read as present.

**Timing.** Steps 5, 9 and 11 are bleed-limited: at 1.918 kΩ and 2200 µF, τ is 4.2 s and a
drain to < 50 counts takes ~13 s. A variant B pad test therefore runs about 40 s, most of
it waiting. Annunciate progress — silence during a 13 s drain will read as a hang.

**The band edges are a bench item, not a desk calculation.** The nominal values (1214,
1037, 0.89 V/ms) are arithmetic and reliable. The acceptable *spread* around them is not:
it stacks 1% resistor tolerance, Schottky V_f variation, the GPIO sag §4 notes as moving
levels by about −0.2 V at 9 mA, and temperature. §S3 warns that telling driven-node levels
apart "does change with the tolerance". Characterise each band across units and across
temperature in Phase 8 before freezing it. Too wide passes a bad board; too narrow scrubs
a good one at the pad.

**Bridgewire current** is ~1.3 mA at steps 8 and 10, the highest of any routine check
(§4's "channel-bias diagnostics about 1.4 mA, 70 times" below no-fire). Steps 2 and 3
put no current through the match at all, because it is isolated.

**Operator handshake.** Steps 1 and 7 wait on a human. Rather than requiring a serial
command, poll for the electrical signature: step 1 completes when both channels read 1214,
step 7 when both read 1037. The board detects the interlock moving instead of being told.
Both waits need a timeout and an abort path.

**Implementation.** A step-indexed state machine driven from `pyro_update()`, never a
blocking call — it spans tens of seconds and two human actions. It asserts `ARM_TOGGLE`
at step 4 only (invariant 13b) and never asserts `FIRE_A` or `FIRE_B` (invariant 13a).

**What the pad test still does not cover:** a low-side path that fails to conduct
(no test may assert `FIRE_x`), a match shorted across the bridgewire at 0 Ω (`DESIGN.md`
§8.2 — visual inspection only), and end-to-end energy delivery (no S1B).

### Success Criteria

#### Automated
- [ ] Host test: the pad test runs to READY on a fully healthy mock
- [ ] Host test: one failure-injection case per failure signature in the table above
- [ ] Host test: step 4 aborts when a channel node rises during the ramp
- [ ] Host test: the step 7 handshake times out rather than hanging
- [ ] Host test: the pad test never writes `FIRE_A` or `FIRE_B`
- [ ] Host test: T4 refuses to run with any interlock condition unmet — one test per row
- [ ] Host test: T4 never writes `FIRE_A` or `FIRE_B`
- [ ] Host test: capacitance inferred from a simulated decay matches, at 100/1000/2200 µF
- [ ] Host test: the pump loop exits within one cycle of an arm condition going false
- [ ] Host test: the trigger fires at ≥ 0.90 × a *measured* vbat, at three different
      simulated pack voltages — a fixed threshold fails the test
- [ ] Host test: precharge timeout aborts to the fault latch
- [ ] A review/grep check that `ARM_TOGGLE` is never bound to a PWM/PIO/timer API

#### Manual
- [ ] Scope `ARM_TOGGLE`: frequency in band, stops immediately on abort
- [ ] Scope U9 EN: settles near 2.7 V while pumping; decays below threshold in ~9.6 ms
      after the toggle stops
- [ ] Scope `SNS_BUS` ramp: 0.89 V/ms across the C_BULK range; U9 never enters current
      limit on inrush
- [ ] **Halt the MCU (debugger break) mid-pump and confirm the bus disarms within ~10 ms**
- [ ] T4 end to end with the mechanical disconnect open: ramp rate, trigger comparison,
      disarm timing and inferred capacitance all correct at the fitted C_BULK
- [ ] T4 refuses to run with a match fitted and the disconnect closed
- [ ] Full pad test end to end on a good board with a real match: READY in ~40 s at 2200 µF
- [ ] Pad test reports the correct step code for a shorted TVS, an absent match and a
      match shorted to ground

---

## Phase 5: `pyro_mk1c.c` — firing sequence F0–F10

### Overview

`IGNITER_OPERATION.md` §3, behind the unmodified `pyro_fire()` / `pyro_update()` /
`pyro_is_firing()` / `pyro_fault()`.

### Changes Required

#### 5.1 Execution model

F0–F8 run inside `pyro_fire()` as a **bounded blocking call** of ~50 ms worst case,
interrupts enabled. F9 (drain, up to 12 s) and F10 (verify) run asynchronously from
`pyro_update()`.

Rationale: a 50 µs sampling cadence cannot be interleaved with `tud_task()` in the
cooperative main loop, and invariant 5 wants the toggle generated from the checking code
path anyway. A ~50 ms stall of USB/network service happens at most twice per flight and
is within ECM and TCP tolerance. Write this rationale into the file — it is the kind of
decision a later reader will try to "fix".

#### 5.2 Sequence

| Step | Action |
|---|---|
| F0 PREP | All bias low; flush ADC filters; latch `SNS_VBAT`; verify §5.1 preconditions |
| F1 ARM | Start the pump (Phase 4) |
| F2 PRECHARGE | Ramp; abort on timeout |
| F3 TRIGGER | At ≥ 0.90 × measured `SNS_VBAT`, assert `FIRE_x`. **Never a fixed delay** |
| F4 PULSE | **Discard the sample at t = fire** — the bridgewire makes an RF burst (§7.4) |
| F5 CLASSIFY | Abrupt stop with charge remaining = fired; decay continuing to zero = misfire still conducting |
| F6 DISARM | **Stop the toggle.** Up to 9.6 ms for EN to collapse. There is no commanded fast disarm |
| F7 HOLD | Hold `FIRE_x` until the `SNS_BUS` slope is flat, 30 ms limit |
| F8 GATE OPEN | De-assert `FIRE_x` at ~0 A |
| F9 DRAIN | Async: watch `SNS_BUS` fall below the indicator threshold. **Skip if the next event is the other channel** |
| F10 VERIFY | Post-fire tracking test; result feeds `pyro_fault()` and the next `pyro_check_continuity()` |

**The armed-window watchdog must outlast the whole sequence.** `wave_capture_arm`
enables a 50 ms watchdog and feeds it from the pump loop. That is safe for the
bench capture, which disables it immediately after, but it does NOT transfer to
F0-F10 unchanged: the pump stops at F6, so nothing feeds the watchdog through
F7's misfire hold, which alone runs up to 30 ms. With F1-F6 taking ~20 ms the
total reaches the 50 ms timeout, and a reset there would both fail to deploy and
open the gate with current still flowing -- violating the F6-before-F8 ordering
the whole shutdown argument rests on. Either size the timeout above the worst
case F0-F10 duration, or feed it from the sequence rather than the pump.

**F6 must precede F8.** Encode the ordering so it cannot be reordered by accident —
separate functions with the dependency in the signature, plus an assertion in F8 that the
toggle is already stopped.

**Never fire both channels at once** (invariant 2): `pyro_fire()` refuses while
`pyro_is_firing()`. `flight_states.c:93,101` already guards on `hal_pyro_is_firing()`, but
the refusal must also live in the backend, since `ground_test.c:105,121` does not check.

**FLT substitute.** `S5 → S7 on FLT` has no hardware source. The remaining detections are
the precharge timeout and the `SNS_BUS` decay profile. A thermal trip now presents as a
precharge timeout or a collapsed bus rather than as an explicit flag; the recovery path is
unchanged, because F6 drops EN and the next F1 raises it.

#### 5.3 This is the only code that may assert FIRE or ARM_TOGGLE

Per invariant 13, `pyro_fire()` and the F0–F10 helpers it calls are the *only* functions
in the tree that write `FIRE_A`, `FIRE_B` or `ARM_TOGGLE` non-zero. `pyro_init()` drives
all three low and never touches them again. Keep the three pin writes in one small static
helper so the grep gate in Phase 7.1 has a single thing to check.

### Success Criteria

#### Automated
- [ ] Host tests drive F0–F10 against a mock: nominal fire, misfire, precharge timeout,
      abort at each step
- [ ] Host test: a second channel is refused while the first is mid-sequence
- [ ] Host test: F8 never runs before F6
- [ ] Host test: the classifier separates fired from misfire on captured decay profiles
- [ ] Host test: `pyro_fault()` reports true after a classified misfire

#### Manual
- [ ] Dummy 1 Ω pulse resistor: correct pulse shape and delivered energy at 1S and 2S
- [ ] No load: post-fire tracking reports absent
- [ ] Scope the gate and the bus through a full misfire hold — the gate opens at ~0 A
- [ ] Reset mid-pulse with the watchdog forced: the TVS clamps and the FET survives
- [ ] Measured worst-case F0→F8 wall time ≤ 50 ms; USB/network recover afterwards

---

## Phase 6: Flight-layer policy — authority, context branch, S8

### Overview

Everything that needs the flight state rather than the hardware. All of it lives above
the HAL, in `flight_states.c` and a new `src/pyro_states.c`.

### Changes Required

#### 6.1 Authority (invariant 11)

The call sites already are the authorities. Make it explicit and checkable:

- `flight_states.c:95,103,153,162` fire only from ASCENT/DESCENT states — the flight
  sequencer
- `ground_test.c:105,121` fire only from PAD_IDLE (already enforced, GND-TEST-04)
- Add an assertion in `try_fire_pyros()` that the current state is a flight state, so a
  future refactor that calls it from PAD_IDLE fails loudly

#### 6.2 The context branch (invariant 12)

The single most important behaviour in the port:

| F10 result | Context | Action |
|---|---|---|
| channel reads **open** | any | fired; log; continue |
| channel still **present** | **flight** | misfire; log; **other channel stays fully available. Never S8** |
| channel still **present** | **ground** | → S8, 60 s timer, fire commands locked out |

Gate on the **landed** state, not on the fire attempt. A drogue misfire entering S8 would
suppress the main, because the 60 s lockout is 6–12× the drogue-to-main window. This
deserves a dedicated host test with an explicit name.

#### 6.3 S8 clearance

Both a timer and a measurement, never one alone. Clear only when 60 s have elapsed **and**
the bus is below threshold **and** still falling **and** no shorted-FET indication, across
several samples. Per invariant 9, one hot-bus sample blocks the clearance regardless of
the median. The timer restarts in full on every fire attempt, test fire, retry, reset and
brownout — never continue a partial count.

S8 needs the bus reading, which `pyro_continuity_t` does not carry. It does not need a new
HAL function: `pyro_fault()` already reports the latched "bus does not decay" diagnosis
from the §8.1 classifier, which is the condition S8 actually gates on.

#### 6.4 Buzzer patterns (§S8)

Distinguished by **rhythm, not pitch**:

| State | Pattern |
|---|---|
| Armed, standby | one short chirp per 5 s |
| Misfire wait running | three quick beeps per 2 s |
| Fault latched | long, short, long, repeated |
| Clear to approach | one slow long tone |

The clear tone sounds only while all conditions hold, and is **never latched**.

#### 6.5 Reconciling the existing flight logic

- `check_pyro_fault()` (`flight_states.c:111-121`) works unchanged — `pyro_fault()` now
  reports the latched classification instead of a FLAG pin, and `EVT_PYRO*_FAULT` keeps
  its meaning.
- `check_post_fire_verify()` (`flight_states.c:124-141`) is F10 by another name. Its
  500–600 ms window now observes the result F10 already computed; verify the timing lines
  up or widen the window.
- `update_continuity_and_buzzer()` (`flight_states.c:179-...`) polls at 1 Hz and reads the
  cached tracking result. No change needed.
- `check_refire()` — see Open Questions.

### Success Criteria

#### Automated
- [ ] **`test_misfire_in_flight_does_not_inhibit_other_channel`** — drogue misfire, then
      main fires within the 5–10 s window
- [ ] Host test: a misfire on the ground enters S8 and locks out fire commands
- [ ] Host test: the S8 timer restarts in full after a simulated reset
- [ ] Host test: one hot-bus sample blocks the clearance
- [ ] Host test: S0 is entered on reset, brownout and watchdog

#### Manual
- [ ] The four buzzer patterns are distinguishable at range by an operator
- [ ] A ground sequence per `IGNITER_OPERATION.md` §10, minus the S1 steps

---

## Phase 7: Cross-flash guards, tests, and documentation

### Changes Required

#### 7.1 Cross-flash guard

- Distinct artifact names (Phase 1.1)
- `PYRO_BOARD_SHORT` in the version string and in `$PYRO` telemetry, so the ground station
  can see which board it is talking to
- The `/api/ota` handler (`http_server.c:455`) rejects an image whose embedded board tag
  does not match the running board, **before** `pfb_mark_download_slot_as_valid()`
- `support/install.py` and `support/update_from_release.py` pick the matching artifact and
  refuse a mismatch

#### 7.1b Invariant 13 gate

A CI check that the only translation unit writing `FIRE_A`, `FIRE_B` or `ARM_TOGGLE` is
`pyro_mk1c.c`, and that within it:

- `FIRE_A` / `FIRE_B` are written only by `pyro_init()` (low) and the F0–F10 helpers
  (invariant 13a)
- `ARM_TOGGLE` is written only by `pyro_init()` (low), the F0–F10 helpers, and T4
  (invariant 13b)

A grep over `src/` for the pin macros is sufficient and cheap.

#### 7.2 Test HAL

`test/hal_test.c:108-135` already satisfies `hal.h` and needs no new functions. It gains a
controllable mock sense layer so the Phase 3–6 host tests can drive tracking counts, decay
profiles and fault injection through the existing `hal_pyro_check()` / `hal_pyro_fault()`.

`sim/hal_sim.c` is untouched.

#### 7.3 Documentation

In this repo:
- `SPECIFICATION.md:50-64` — the pinout table is stale even for MK1B (it lists I2C on
  GPIO 8/9 and a GPIO 8 test jumper that does not exist). Correct it and add an MK1C table.
- `SPECIFICATION.md:77-90` — the TEST_MODE description references the same absent jumper.
- `README.md`, `PORTING.md`, `ARCHITECTURE_V2.md` — board-variant build instructions, and
  a note that `hal.h` / `pyro.h` are the porting contract.
- `REQUIREMENTS.md` / `TRACEABILITY.md` — requirement IDs for the MK1C states and
  invariants, traced to `DESIGN.md` §9 and `IGNITER_OPERATION.md` §8.

In `~/Documents/pyro_mk1c/` (separate repo — coordinate with the user before editing):
- `DESIGN.md` §S10: GPIO25 → GPIO8 for D1.
- `DESIGN.md` §12: the ADC budget is closed — ILM dropped, SNS_VBAT kept.
- `DESIGN.md` §5, §5.0, §7.2, §8.1, §S1 step 6: mark the ILM and FLT rows as
  not-implemented-in-hardware on this revision.
- `DESIGN.md` §S8: remove the claim that §7.2 prevents an out-of-spec capacitor from
  silently invalidating the ALL CLEAR time (Phase 4.2).
- `IGNITER_OPERATION.md` §7 and §9: MCU pins **are** now assigned — replace the "not yet
  assigned" note with the pin table; drop ILM and FLT from the signal table.
- `IGNITER_OPERATION.md` §2.2: S10 heartbeat pin correction.
- Both documents: **delete S1 and S1B**, add invariants 13a and 13b, and record T4 as the
  one optional test that energises the bus. Rewrite §4 of
  `IGNITER_OPERATION.md` ("Ground checks before flight") around the bus-cold set, and move
  the three residuals of Phase 3.6 into §8.4's latent list.
- `DESIGN.md` §3 and §10: reconcile R_BLEED with the fabricated single R103 2.2 kΩ.
- `DESIGN.md` §S9 and §S8: the indicator network is not fitted; the approach rule's
  "look at the indicator" step has no lamp to look at.

### Success Criteria

#### Automated
- [ ] Both board builds produce distinctly named artifacts
- [ ] Host tests pass with `-DPYRO_BOARD_MK1B` and with `-DPYRO_BOARD_MK1C`
- [ ] CI (`.github/workflows/build.yml`) builds both variants
- [ ] `git diff` against `src/hal.h` and `src/pyro.h` is empty for the whole branch

#### Manual
- [ ] An MK1B image offered to an MK1C board over OTA is rejected with a clear message
- [ ] No document still claims D1 is on GPIO25 or that a pin map does not exist

---

## Phase 8: Bench verification

`DESIGN.md` §11 is the authority. The firmware-dependent items, as acceptance gates:

- [ ] Tracking-test margin with a real match and with an open channel, across temperature
- [ ] Precharge ramp at 0.89 V/ms across the C_BULK range; U9 never enters current limit
- [ ] Gate hold vs EN decay; order margin ≥ 1.5× on three units
- [ ] Bleed decay at max C_BULK, against the S8 timer
- [ ] Bias GPIO current with the bus at 8.4 V — confirms the Schottky blocks
- [ ] ADC settling after a bias pulse, inside the 5 ms budget
- [ ] Full misfire hold at max C_BULK through U9: no trip, or a clean thermal trip
- [ ] Reset mid-pulse with the watchdog forced: TVS clamps, FET survives
- [ ] No-fire margins of the bias currents against the most sensitive supported match
- [ ] Indicator check with the av-bay assembled, including remote LED polarity

The open-R_BLEED check no longer needs S1 — T2 reads it directly (Phase 3.5). The
stuck-open FET check and the end-to-end firing-path test have no bench substitute; see
Phase 3.6.

---

## Testing Strategy

The existing host-test harness (`host_tests`, `integration_tests`, `closedloop_tests`)
compiles flight logic against `test/hal_test.c`. Because `hal.h` does not change, that
harness keeps working untouched, and the new Phase 6 policy tests slot straight into it.

**Testable on the host**: ADC scaling, the §8.1 classifier, outlier/latch rules
(invariants 8, 9), the F0–F10 sequencer against a mock, the context branch (invariant 12),
the S8 timer and clearance logic.

**Requires hardware**: pump frequency and EN settling, the precharge ramp rate, gate/EN
shutdown ordering, decay-profile classification against real captures, ADC settling, and
everything in Phase 8.

**Never tested with a live match**: every electrical test above runs with the match
disconnected or with a dummy load.

---

## Open Questions

1. **R_BLEED is one resistor, not two** (R103 2.2 kΩ). `DESIGN.md` §3 and §10 require a
   split pair so no single open resistor can strand the bus charged. Hardware change, or
   accept and update the documents? Firmware detects the open either way (T2), but only
   while it is alive — which is precisely the case the split was meant to cover.
2. **The §S9 armed-and-active indicator is not fitted.** §S8's approach rule ends with
   "look at the indicator; a lit lamp means do not approach", and §S8 is explicit that a
   shorted high side holds the bus up indefinitely so no wait clears it. Without the lamp
   that hazard has no annunciator the operator can trust, since firmware may be dead.
   Hardware change, or a documented procedural substitute?
3. **A non-conducting low-side path is undetectable before flight.** The safety-critical
   stuck-on mode is caught by T3; this residual is the misfire mode only. Accepted
   consequence of invariant 13 — confirm, and decide whether it goes on the range checklist
   as a known latent.
4. **`check_refire()` vs invariant 4.** `flight_states.c:145-163` re-fires a channel
   1–1.5 s after the first attempt if still ballistic (PYR-REFIRE-01). `DESIGN.md`
   invariant 4 says all faults latch and nothing retries automatically. My reading is that
   these are compatible — invariant 4 is about *fault* latching, not misfire recovery — so
   `check_refire()` should survive on MK1C as a second F0–F10 pass, with each attempt
   restarting the S8 timer. Needs confirmation before Phase 6.
5. **Build variant A vs B** (`DESIGN.md` §1.1, 1S/100 µF vs 2S/up to 2200 µF) changes the
   precharge timeout and the F7 hold bound. With ILM gone the firmware cannot infer the
   fitted capacitance during the ramp, so the variant must be a config field or a
   compile-time constant. **T4's decay measurement now provides the cross-check** (Phase
   4.3), but only when an operator runs it — so the question narrows to whether T4 should
   be mandatory after any change to C_BULK.
6. **GPIO22 (`GPIO`, J1.6)** is the only spare pin. Reserve it, or leave it for the user?
7. **GPIO0/GPIO1 dual-use.** The nets are labelled `TX0/SDA0` and `RX0/SCL0` with 4k7
   pull-ups fitted, so J1 can be a UART or an I2C0 bus. Firmware assumes UART. Is I2C0 on
   J1 a planned mode?
8. **Landing detection during a live sequence** — `IGNITER_OPERATION.md` §9 flags this as
   unspecified. Suggested rule: latch the context at F0 and do not re-evaluate mid-sequence.

---

## References

- `~/Documents/pyro_mk1c/DESIGN.md` draft 0.5 — hardware, §4 numbers, §8 FMEA, §9 invariants
- `~/Documents/pyro_mk1c/IGNITER_OPERATION.md` draft 0.1 — §2 S-states, §3 F-states, §6 context branch
- `~/Documents/pyro_mk1c/pyro_mk1c.kicad_sch` — netlist source of truth for the pin map
- `src/hal.h`, `src/pyro.h` — the frozen contracts this port implements against
- `src/hal_hardware.c:294-322` — the existing delegation seam
- `DECISIONS.md` #1 (integer-only math), #8 (switch dispatch with a safe default)
- `PORTING.md` — existing HAL porting guidance
