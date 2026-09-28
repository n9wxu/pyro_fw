# Pyro MK1C — Theory of Operation

How the MK1C flight computer works, from the pins up: what the hardware does,
what the firmware in this directory does with it, and why each number is what
it is. The code refers here instead of carrying the explanation itself;
`support/trace_check.py` fails CI if a comment's `See THEORY_OF_OPERATION.md
"..."` names a heading this file does not have.

The board's design documents live with the KiCad project, outside this
repository: `~/Documents/pyro_mk1c/DESIGN.md` (hardware, levels, FMEA,
invariants) and `IGNITER_OPERATION.md` (the S0–S8 states and the F0–F10
firing sub-machine). Where this file and they disagree, this file describes
the board as built and measured; the differences are listed in
[Known limits](#known-limits).

## Overview

MK1C fires two electric matches from one armed firing bus:

- **One shared high side.** U9, a TPS259570 eFuse, connects the pack to the
  firing bus only while a charge pump holds its enable up. The pump runs only
  while the firmware keeps feeding it, so a stopped processor disarms the bus
  by construction.
- **One low side per channel.** Q103 and Q104 (AO3400A) close a match's return
  to ground. A match fires only when the bus is armed **and** its own gate is
  on.
- **Bias-injection sensing.** Three GPIOs can each push a small current into a
  node (the bus, channel A, channel B) through a Schottky and 330 Ω, and four
  dividers bring the pack, the bus and both channel nodes to the ADC. Presence
  is read by biasing the bus and seeing which channel follows it.

Between fires the firmware checks two things and nothing else: that each match
is present, and that nothing is shorted (DD-055). A fire is the sequence of
DESIGN.md 7.1 run one step per loop iteration (DD-056).

## Hardware

| Item | Part | Notes |
|---|---|---|
| MCU | RP2040, QFN-56, 12 MHz crystal | bare chip; its own SDK header, `sdk/pyro_mk1c.h` |
| Flash | XT25F128FWOIGT-W, 16 MB | 8 MB littlefs, the rest two OTA slots behind `pico_fota_bootloader` |
| High side | U9 TPS259570 eFuse | current limit 4.05 A (R_ILIM 499 Ω); output ramp 0.89 V/ms (C_dVdT 47 nF); latches off on overtemperature |
| Arm pump | C101 10 nF, BAT54S, C_HOLD 100 nF, R_BLEED_EN 100 kΩ | ARM_TOGGLE → U9 EN/UVLO |
| Low sides | Q103, Q104 AO3400A | gates through R108/R113 100 Ω |
| Bias sources | D104/D106/D107 BAT54WS, R112/R117/R120 330 Ω | from BIAS_A, BIAS_B, BIAS_BUS |
| Sense dividers | 10 k / 4.99 k (bus, A, B); 100 k / 49.9 k (pack) | all 0.3329 |
| Bus bleed | R_BLEED, two 4.7 kΩ in parallel | 2.35 kΩ; no single open resistor removes it |
| Bus capacitance | C115 and strays, 1.1 µF | **no bulk capacitor** is fitted |
| Pressure | U2 MS5607, I2C | PS tied high (I2C mode), CSB low |
| Buzzer | BUZZER1 KXG0903C3, from VIN | switched on its low side by Q2 AO3400A |
| LED | D1, blue | |

U9's ~FLT goes only to a pull-up and its ILM only to R130 499 Ω; neither reaches
the MCU, so the firmware has no shutdown flag and no load-current reading.

## Pins

Checked against `pyro_mk1c.kicad_sch` by netlist export
(`kicad-cli sch export netlist --format kicadsexpr`), not from the schematic
images. `board_pins.h` is the map; `pin_caps.h` says what each pin may become;
`board_info.c` names the pyro pins for `picotool info -a`.

| GPIO | Signal | Goes to |
|---|---|---|
| 0, 1 | UART0 TX, RX | J1.4, J1.5 — telemetry and ground-test commands |
| 6, 7 | I2C1 SDA, SCL | MS5607, R3/R5 4k7 pull-ups |
| 8 | LED | R4 1 kΩ → D1 |
| 11 | buzzer | Q2 gate |
| 12 | ARM_TOGGLE | C101 → the pump → U9 EN |
| 16 | BIAS_A | channel A node |
| 17 | FIRE_A | Q103 gate |
| 18–21 | user pads | J3.3–J3.6, Lua; J3.1 is 3V3, J3.2 ground |
| 22 | spare | J1.6 |
| 23 | BIAS_BUS | the firing bus |
| 24 | FIRE_B | Q104 gate |
| 25 | **BIAS_B** | channel B node — **not an LED** |
| 26–29 | ADC0–3 | pack, bus, channel A, channel B |

**GPIO25 is a bias injector.** MK1A and MK1B put their LED there, and the stock
`boards/pico.h` names it `PICO_DEFAULT_LED_PIN`, which TinyUSB's BSP drives as an
output in `board_init()`. That is why this board has its own SDK header, which
must never define `PICO_DEFAULT_LED_PIN`.

## Start-up

`hal_platform_init()` silences the buzzer, then calls `board_early_init()`,
which drives ARM_TOGGLE, both gates and all three bias pins low before USB,
networking or the filesystem start. The pads' reset state (input, pull-down)
already holds them there; this makes it explicit. `pyro_init()` repeats it,
claims the four ADC inputs, loads the arm pump's PIO program and claims its
state machine — at boot, so a full PIO fails there and not at a fire — and takes
the first quiescent reading.

## The main loop

The shared loop (`src/main_hardware.c`) runs every 20 ms (`src/loop_period.h`,
DD-065). The MS5607's one-shot conversion is started first; then platform
services, the flight state machine, and the outputs, where `hal_pyro_update()`
calls `pyro_update()`. Each `pyro_update()`:

1. reads the pack, the bus and both channel nodes with no stimulus
   (`read_quiescent()`);
2. advances the firing sequence, if one is running;
3. otherwise advances the presence test;
4. counts toward the bus-hot fault;
5. every 5 s, sends a `!PYRO q[...] trk[...]` status line.

Nothing waits (DD-053): every settle is a deadline a later iteration checks.
The loop's watchdog, at twice `PYRO_LOOP_WORST_MS`, is the only watchdog.

## Firing bus

```
 VBAT ── U9 eFuse ──┬── FIRING BUS ──┬── match A ── node A ── Q103 [FIRE_A] ── GND
          ▲ EN      │                └── match B ── node B ── Q104 [FIRE_B] ── GND
          │         ├── R_BLEED 2.35k ── GND
   ARM_TOGGLE pump  ├── divider ── ADC1
                    └── BIAS_BUS injector
```

The mechanical arm disconnect, in the rocket, breaks both match leads. With it
open no electrical fault fires a match.

**U9 conducts backwards while off.** Above about 0.72 V the bus drives current
back into U9's OUT — a junction behind about 250 Ω, measured on the bench
(DD-054). It is inside the part and cannot be removed, so the design works
around it: under bias the bus sits at about 688 counts (1.66 V), not the
1058 counts DESIGN.md 4 derives, and that level moves with the part and its
temperature. No decision is taken on the bus's absolute level.

## Sense network

Every divider is 0.3329, so against the 3.3 V reference one count is 2.421 mV
at the node, 413 counts a volt (`counts_to_node_mv()`, with build-time checks
against DESIGN.md 4's levels in `pyro_sense.h`). Each read is the median of
three conversions, which drops one that lands on a switching edge.

| Reading | Meaning |
|---|---|
| quiescent bus near 0 | the bus is cold |
| quiescent bus above ¾ of the pack | the high side is on, or shorted |
| biased bus about 688 | a healthy bus under bias |
| biased bus under 200 | the bus did not rise: something shorts it to ground |
| biased channel near the bus | a match joins that channel to the bus |
| biased channel under 50 | no match, or an open one |

A bias source is a 3.3 V GPIO through a BAT54WS and 330 Ω. Through a fitted
match the tracking current is about 0.11 mA on the bench — far under a
100 mA no-fire current.

## Presence test

Every 500 ms, while no fire is in progress, the firmware biases the bus
(BIAS_BUS only), waits one loop for it to settle, reads the bus and both
channels, and lets the bias go (`tracking_step()`). A channel reading at least
half the bus has a match across it (`track_channel()`): a match reads about the
whole bus, a 5 kΩ dirty connector about three quarters, and the raw counts are
reported so a degraded joint shows. A test whose bus stayed under 200 counts is
no reading at all.

With 1.1 µF on the bus, one loop is ample settle. The pulse is 20–21 ms where
DESIGN.md S3 asks 5–10 ms, because the loop ends a phase no sooner than its
next iteration (task P1: a timer one-shot, decided).

The test never runs while the bus is live: not during a fire, and after one
not until the bus has drained cold (DESIGN.md invariants 1 and 7).

## Short latches

Three faults latch until reset (invariant 4), each only after agreeing samples
(invariant 8):

| Fault | Seen when | Samples |
|---|---|---|
| bus hot | the quiescent bus is above ¾ of the pack, the pack present, and the sequence did not charge it | 3 loops |
| bus shorted | a presence test's bus stays under 200 counts | 3 tests |
| precharge timeout | a fire's bus did not reach the pack in time | 1 |

"The sequence charged it" covers a fire and the 100 ms after its pump stops:
the bus's 1.1 µF bleeds through 1.85 kΩ on a 2 ms constant, so a bus still
hot after that is a high side that did not turn off. A bus short covers the
bus itself, a harness lead to ground, and a shorted low-side FET behind a
fitted match.

`pyro_fault()` reports any latched fault for both channels: the bus is theirs in
common. `pyro_get()` never reports a short; a short is the bus's.

## Arm pump

```
ARM_TOGGLE ──┤├── C_PUMP 10n ──┬──▶|──┬── C_HOLD 100n ── GND
                               │ D2   ├── R_BLEED_EN 100k ── GND
                              ─┴─ D1  └── U9 EN
```

Each rising edge moves charge into C_HOLD; the hold settles near 2.7 V, about
twice U9's worst-case 1.27 V threshold, and the pump has about ten times the
bleed's current at 10 kHz. Stopping the toggle **is** the disarm: C_HOLD bleeds
and U9 turns off within 9.6 ms (DESIGN.md 5.1).

The toggle comes from a PIO state machine that is FIFO-paced (`arm_pump.pio`):
each word pushed buys one burst of 50 cycles (5 ms), and the machine stalls when
the words run out. A free-running PWM would keep a dead processor armed; this
cannot run past what the firmware last pushed. `arm_pump_feed()` tops the FIFO
up once a loop, never waiting. Four FIFO words and the one running hold 25 ms,
a loop and a quarter, which carries the pump across a loop period and bounds
how far it runs past the last check (`arm_pump.h` sizes the burst from the
period). A loop that stops leaves the bus disarmed within about 35 ms: the 25 ms
the FIFO holds, then U9's 9.6 ms.
`arm_pump_stop()` disables the machine and returns the pad to plain GPIO, low.

## Firing sequence

`pyro_fire()` asks `sequence_refusal()` first. A fire is refused, with its reason
on the telemetry, unless:

- the channel is 1 or 2, and no fire is in progress;
- no fault is latched;
- a presence test has run, since this channel last fired, and read it present;
- the pack is above 3.0 V (an empty 1S cell; U9's own lockout is 2.5 V);
- the bus is not hot — unless a fire just left it charged, since a second event
  does not wait for the drain (DESIGN.md 5.3).

A refusal leaves `pyro_is_firing()` false, and the flight records a refusal
rather than a deployment. Accepted, the fire runs one step per loop:

| Step | DESIGN.md 7.1 | What happens | Leaves when |
|---|---|---|---|
| arm | F0–F1 | the bias drops; the pump starts and is fed | at once |
| precharge | F2 | each loop re-checks the faults, then feeds the pump; U9 ramps the bus at 0.89 V/ms | the bus is at 90 % of the measured pack, or the timeout |
| fire | F3, F6 | the gate goes on and the pump stops in the same step | — |
| hold | F7 | the gate stays on | the bus is flat (25 counts between loops) once U9's enable has had 10 ms to collapse, or 30 ms |
| release | F8 | the gate goes off | — |
| drain | F9 | the bleed empties the bus; the presence test waits | the bus is cold (50 counts) |
| verify | F10 | the next presence test reports `open: fired` or `still present: misfire, treat as live` | — |

**The pulse is U9's current limit.** No bulk capacitor is fitted, so there is
no stored charge to dump: with the gate on, U9 drives about 4 A into a 1 Ω
bridgewire for the 9.6 ms its enable takes to collapse after the pump stops —
far more than the 15 mJ a match needs (DESIGN.md 1.2, M5). A misfire holds U9
in its limit for the same 9.6 ms and no longer.

**The precharge timeout** is 1.5 times the pump's 0.4 ms and the ramp to 90 %
of the measured pack: about 14 ms on 2S. The bus not there by then aborts the
fire, drives no gate, and latches a fault. A misfire, by contrast, latches
nothing: it never inhibits the other channel (invariant 12). What follows a
misfire on the ground (S8) is the flight code's decision.

Two channels fire one after the other: the second may start as soon as the
first releases its gate, on the bus the first left charged. Never both at once
(invariant 2).

## Flash and the fire

A flash sector erase stalls the loop for tens of milliseconds — longer than the
pump coasts — and a precharge that stalls times out and latches. So
`board_flash_ok()` answers false from the command until the gate is released,
and the main loop opens no flash window meanwhile. The fire takes a few loop
periods; the log's writes wait for them.

## Pressure sensor

One MS5607 on I2C1, at its fastest, 400 kHz: R3 and R5 (4k7) hold fast mode's
300 ns rise to about 75 pF (DD-052). Bring-up is the shared single-sensor path,
`src/pressure_single_sensor.c`: clock the bus free in case the sensor was left
mid-transfer across a reset, hand it to the I2C block, let the pull-ups settle,
detect. In flight the sensor converts a pressure and then a temperature every
loop, started at the top of the loop, each read 9.1 ms after its command by a
one-shot alarm whose handler runs from RAM (DD-051, DD-066), so a flash write
cannot delay a read or its timestamp. The pair is ready 18.6 ms after the top,
1.4 ms before the next; at standard mode it would be 0.26 ms.

Every transfer gives up rather than wait on a part holding the bus: detection
and the one-shot's transfers within 2 ms (DD-069). A conversion a flash erase
or program ran beside is discarded, not used, and counted in `pres_flashed` on
`/api/status` (DD-068). The buzzer does not disturb the sensor: on the bench
the scatter is 6–7 Pa beeping or not, where MK1B's rises during a beep code
(DD-068, task B-BZ).

## Telemetry, LED and buzzer

The defaults in `src/hal_common/board_defaults.c` drive all three: UART0 at
115200 on GPIO0/1 for telemetry and ground-test commands, the LED on GPIO8
lit from boot and toggled by the main loop's pressure task every fifth reading
— never from a timer, which would keep blinking after the firmware stopped —
and the buzzer on GPIO11, which an operator may move to another pad in
`pins.ini`.

## Lua and released pads

Lua runs on core1 and owns the four J3 pads (GPIO18–21); `pins.ini` can give
it the spare, GPIO22 on J1.6, too. A pyro channel can be released to Lua;
`pin_caps.h` lists what each pyro pad may then become. ARM_TOGGLE carries no
plain-output capability: holding it at a level does not hold U9 on, so this
board cannot offer a half-bridge. With both channels released,
`hal_pyro_update()` stops calling `pyro_update()`: there is nothing to arm and
nothing to sense.

## Ground test switch

A switch held closed at power-up puts the board in ground test mode, and
opening it fires the enabled channels on a countdown (DD-071). J3 has its own
ground, so a switch from one J3 user pad to J3.2 suits this board:
`ground_test=ground` and, say, `ground_test_pin=18` in `pins.ini`. Any other
digital pad serves, GPIO22 included, and two of them can carry the two-pad
wiring (`ground_test=pair`). The switch's pads leave Lua.

The procedure fires through `pyro_fire()`, so the refusals of
[Firing sequence](#firing-sequence) still apply: a channel the presence test
has not read present is refused, the refusal is reported, and the procedure
goes on.

## Faults

| Condition | What the board does |
|---|---|
| high side shorted | the quiescent bus reads hot: latched |
| bus, harness or a low side behind a fitted match shorted to ground | the biased bus will not rise: latched after three tests |
| a match absent or open | reads open; a fire on it is refused |
| the bus will not charge during a fire | precharge timeout: aborted, latched |
| U9 will not turn off after a fire | the bus stays hot past the bleed: latched |
| a misfire | reported by the verify step; nothing latched |
| the loop stops mid-arm | the pump stalls; the bus disarms within about 35 ms |
| the processor resets mid-pulse | the pads' pull-downs open the gates; the match's TVS takes the transient (DESIGN.md 5.4) |

## Known limits

- **No bulk capacitor** is fitted: DESIGN.md 1.1's variants and 7.3's
  capacitor-discharge pulse do not describe the board.
- **U9's reverse conduction** sets the bus's level under bias (DD-054); an open
  R_BLEED does not show in it, and the firmware does not look for one
  (DD-055).
- **No ~FLT, no ILM**, so DESIGN.md 7.2's capacitance check and the FLT abort
  have no source; the precharge timeout covers a loaded bus.
- **The presence pulse is 20–21 ms**, not 5–10 (task P1).

## Build

`board.cmake` selects the SDK header, the 16 MB geometry and Lua, and declares
the loop budget, `PYRO_LOOP_WORST_MS` (500 ms: a sector erase can take hundreds
of milliseconds on this class of part). `/api/status` reports `loop_max_us` and
`loop_overruns`, so the budget is checked rather than asserted.

| File | Holds |
|---|---|
| `pyro_board.c` | `src/pyro.h`, `board_early_init()`, `board_pyro_raw()`, `board_flash_ok()` |
| `pyro_measure.c` | the quiescent read and the presence test |
| `pyro_faults.c` | the latched bus faults |
| `pyro_sequence.c` | the firing sequence |
| `pyro_sense.h` | levels, scaling and the presence verdict — shared with the plant model |
| `arm_pump.c`, `arm_pump.pio` | the charge pump |
| `board_info.c` | picotool's pin names |

## Tests and models

- `board_pyro_mk1c_tests` runs this directory's real code on the host against
  `sim/hw/` (a Pico SDK stand-in, PIO pump included) and the plant model of the
  board as measured (`sim/plant/plant_mk1c.c`): presence, the latches, every
  refusal, a fire on 2S and 1S, the measured trigger, the pump's bounds, a
  stopped loop, an aborted precharge, a misfire, both channels in turn, a high
  side stuck on, and the flash window.
- `plant_tests` holds the model to the bench measurements.
- `sensor_bringup_tests` and `ms5607_tests` cover the pressure sensor, the
  one-shot at this board's 400 kHz.
- `integration_tests` flies the flight software built for MK1C.
- `boards/sim_mk1c` runs the whole flight software against the same model.

## References

- `~/Documents/pyro_mk1c/DESIGN.md`, `IGNITER_OPERATION.md`, `pyro_mk1c.kicad_sch`
- `docs/datasheets/TPS2595_SLVSE57C_2018-04.pdf` (U9), `MS5607-02BA03_2017-06.pdf`,
  `rp2040-datasheet_2025-02-20.pdf`, `UM10204_I2C-bus_Rev7.0_2021-10.pdf`
- DD-051 (the MS5607 one-shot), DD-052 (bus speeds), DD-053 (no waits),
  DD-054 (the bus as measured), DD-055 (presence and shorts only), DD-056 (the
  fire), DD-065 (the 20 ms loop), DD-066 (a pair every loop), DD-068
  (conversions beside a flash operation), DD-069 (bounded transfers), DD-071
  (the ground test switch) in `DECISIONS.md`
