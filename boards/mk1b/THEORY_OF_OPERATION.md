# Pyro MK1B — Theory of Operation

How the MK1B flight computer works, from the pins up. The code refers here
instead of carrying the explanation itself; `support/trace_check.py` fails CI
if a comment's `See THEORY_OF_OPERATION.md "..."` names a heading this file
does not have.

The design is `~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch`; the sense network
below was read from its netlist
(`kicad-cli sch export netlist --format kicadsexpr`), as was the model in
`sim/plant/plant_mk1b.c`.

> **As built, MK1B cannot sense continuity.** Both JLCPCB builds fit U5 as the
> AP2192**A**, whose outputs discharge to ground while disabled and hold the
> sense nodes near 0 V. See [Known limits](#known-limits) and task B-U5.

## Overview

MK1B is a Raspberry Pi Pico module on a carrier board. It fires two igniters
through a two-key circuit, the same topology as MK1A: a switched high side per
channel — the two halves of an AP2192 dual high-side switch — and one shared
low side. Current flows through an igniter only when its high side **and** the
shared low side are on.

The shared low side is also the continuity stimulus: with both high sides off,
switching it on lets a connected igniter pull its sense node to ground against
a weak pull-up. The AP2192 adds a fault flag per channel that MK1A lacks.

## Hardware

| Item | Part | Notes |
|---|---|---|
| MCU | Raspberry Pi Pico module (RP2040) | the SDK's own `pico` board header |
| Flash | 2 MB, on the module | 984 KB littlefs, the rest two OTA slots |
| High sides | U5 AP2192AMPG-13 (LCSC C507872) | EN1/EN2 gate OUT1/OUT2; FLG1/FLG2 open drain |
| Low side | Q1B, the second FET of an AO6800 | gate `sw_gnd` |
| Fuse | F2, 1.5 A PTC | resets |
| Sense | R26/R19 100 kΩ to 3V3; R25/R18 100 Ω; C25/C21 100 nF | per channel |
| Flag pull-ups | R21/R20 100 kΩ | |
| Pressure | a BMP280 or an MS5607, on separate SDA pads | a board carries one |
| Buzzer | LS1 | |
| LED | the Pico's own | |

## Pins

`board_pins.h` is the map, `pin_caps.h` says what each pin may become, and
`board_info.c` names the pyro pins for `picotool info -a`. `PICO_BOARD` is the
SDK's `pico`, so those pin names and the program name are what identify the
board.

| GPIO | Signal | Goes to |
|---|---|---|
| 0, 1 | UART0 TX, RX | the TRRS jack |
| 6 | I2C1 SDA | the BMP280 pad — no pull-up fitted |
| 7 | I2C1 SCL | both sensors, R10 4k7 |
| 8 | user pad | J1, Lua |
| 10 | I2C1 SDA | the MS5607 pad, R11 4k7 |
| 15 | PYRO_COMMON_EN | Q1B gate — the shared **low** side |
| 16 | buzzer | LS1 |
| 17 | PYRO1_FLAG | U5 FLG2, active low |
| 18 | PYRO2_FLAG | U5 FLG1, active low |
| 21 | PYRO1_EN | U5 EN2 → OUT2 → channel 1 (CN1.1, drogue) |
| 22 | PYRO2_EN | U5 EN1 → OUT1 → channel 2 (CN1.4, main) |
| 25 | LED | the Pico's LED |
| 26, 27 | ADC0, ADC1 | SENSE1, SENSE2 |

The name PYRO_COMMON_EN reads as a high-side enable; the netlist puts it on
the low-side FET's gate.

## Start-up

`hal_platform_init()` silences the buzzer, then `board_early_init()` drives
PYRO_COMMON_EN and both enables low before USB, networking or the filesystem
start. `pyro_init()` repeats it, claims the sense inputs and pulls the two
flags up. The first continuity check starts from `pyro_update()`, never from
`pyro_init()`: a board with both channels released never calls
`pyro_update()`, so the common — by then Lua's pad — is never left raised.

## The main loop

The shared loop (`src/main_hardware.c`) runs every 20 ms (`src/loop_period.h`, DD-065) and calls
`pyro_update()` from its outputs stage. `pyro_update()` either ends a fire
pulse or advances the continuity check. Nothing waits (DD-053).

## Pyro circuit

```
 +3V3 ── R26 100k ──┬── Switched_BAT1 ── CN1.1 igniter 1 CN1.2 ──┐
                 U5 OUT2 ── R25 100R ──┬── ADC0                    │
                                    C25 100n                      ├── F2 1.5A PTC ── Q1B [PYRO_COMMON_EN] ── GND
 +3V3 ── R19 100k ──┬── Switched_BAT2 ── CN1.4 igniter 2 CN1.3 ──┘
                 U5 OUT1 ── R18 100R ──┬── ADC1
                                    C21 100n
```

`pyro_fire()` is the only function that raises an enable, and only with the
common. No check or boot path raises one.

## Continuity check

Once a second, in two steps:

| Step | Common | Reads |
|---|---|---|
| shorts | off, after the idle second | a channel still low has a path to ground bypassing the low side: a short |
| presence | on, for its 10 ms settle, read at the next loop | a channel pulled low has an igniter; one left high is open |

The node behaves as MK1A's: against the 100 kΩ pull-up a fitted igniter reads
about 0 counts, a 1 kΩ bad joint 41, a 10 kΩ leak 372, and nothing 4095. A
channel is good under 500 counts with no short, open over 3000, and between
the two it is neither; `pyro_get()` reports the presence count so a degraded
joint shows (DD-059). Until the first check completes every channel reads open,
and the first waits 50 ms from boot for the node to charge (five times
100.1 kΩ × 100 nF).

The high sides are held off during a check, but only the ones this board
still owns: a released channel's enable is a Lua pad, and writing it would
stamp a Lua output low on every check.

## Firing

`pyro_fire()` turns the common and the channel's enable on and starts a
500 ms pulse; the check is suspended, since the pulse owns the common. When the
pulse ends, the enable goes off and the common stays on as the stimulus, so a
fresh presence reading lands a loop or two later, about 40 ms — inside the
flight's post-fire verify window, which opens as the pulse ends and runs 100 ms. With the common on there is no short
reading; the last one stands.

## Pressure sensor

Two SDA pads share SCL (GPIO7) on I2C1, a BMP280's on GPIO6 and an MS5607's on
GPIO10, and a board carries one or the other. `pressure_board.c` probes both
as loop steps (DD-053):

1. clock the bus free on **both** pads — either sensor may be the one left
   mid-transfer across a reset;
2. the BMP280's pad at 100 kHz: soft-reset at both of its addresses, wait out
   its 2 ms start-up, detect;
3. if none, release that pad and try the MS5607's at 400 kHz.

The speeds come from the board (DD-052): SCL and the MS5607's SDA have 4k7
pull-ups (R10, R11), which carry fast mode; the BMP280's SDA has only the
RP2040's own 50–80 kΩ, too slow an edge for fast mode, so its probe stays in
standard mode. The MS5607 then converts a pressure and a temperature every
loop, read from a one-shot alarm whose handler runs from RAM (DD-051, DD-066):
the pair is ready 18.6 ms after the top of a 20 ms loop, which only fast mode
allows. A BMP280 fitted instead converts once a loop, commanded in forced
mode and taken at the next (DD-067).

## Telemetry, LED and buzzer

The defaults in `src/hal_common/board_defaults.c` drive all three: UART0 at
115200 on the TRRS jack, the Pico's LED lit from boot and toggled from the main
loop, and the buzzer on GPIO16, which an operator may move in `pins.ini`.

## Lua and released pads

Lua owns GPIO8. GPIO0/1 can be moved to Lua too, but that costs the telemetry
downlink, and nothing hands the UART back if the script dies. A pyro channel
can be released to Lua — the bench MK1B has both released — and a released
enable with a released common is a half-bridge on PIO0, the pyro block. The
F2 PTC and the AP2192's current limit make a shoot-through trip and recover,
which is why this board's protection class is `PYRO_PROT_PTC_LIMITED`. With
both channels released, `hal_pyro_update()` stops calling `pyro_update()`.

## Faults

| Condition | What the board does |
|---|---|
| an overcurrent or overtemperature in U5 | its flag goes low; `pyro_fault()` is true |
| an igniter absent or open | reads open — see [Known limits](#known-limits) |
| a lead shorted to ground | reads shorted — see [Known limits](#known-limits) |

## Known limits

- **U5 is the AP2192A (task B-U5).** The A variant discharges each output to
  ground through about 100 Ω while it is disabled (DS32193 page 4, R_DIS, and
  its note 6). Against the 100 kΩ pull-ups that holds both sense nodes at a few
  millivolts whenever a channel is off — which is always, during a check. A
  fitted igniter, an empty connector and a real short all read the same, near
  0 counts, and the check reports every channel shorted. The flight fires only
  a channel with continuity (PYR-SAFE-01), so an MK1B that owns its pyros never
  deploys. The base AP2192 (DS31569) has no discharge and the same MSOP-8EP
  footprint; with it the check above works as written.
- **The BMP280 pad has no pull-up**, so a BMP280 board's sensor runs at
  100 kHz.

## Build

`board.cmake` selects the stock `pico` header, the 2 MB geometry and Lua, and
declares the loop budget, `PYRO_LOOP_WORST_MS` (500 ms, dominated by a flash
sector erase). `/api/status` reports `loop_max_us` and `loop_overruns`.

| File | Holds |
|---|---|
| `pyro_board.c` | `src/pyro.h` and `board_early_init()` |
| `pressure_board.c` | `src/pressure_sensor.h`: two sensors, one SCL |
| `board_info.c` | picotool's pin names |

## Tests and models

- `board_pyro_tests` runs the real `pyro_board.c` on the host against
  `test/fake_sdk`, with the sense node modelled from the netlist: the check's
  timing, one reading a second, igniter, empty, short and a bad joint, a fire
  and its fresh reading, and a released enable left alone.
- `sensor_bringup_tests` and `ms5607_tests` cover the two-pad bring-up and the
  MS5607.
- `plant_tests` models the sense network from the netlist; it leaves out U5's
  discharge.
- `boards/sim_mk1b` runs the real `pyro_board.c` against the plant.

## References

- `~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch`, and the JLCPCB BOMs beside it
- Diodes DS32193 (AP2182A/AP2192A) and DS31569 (AP2182/AP2192), from
  diodes.com
- `docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`, `MS5607-02BA03_2017-06.pdf`,
  `UM10204_I2C-bus_Rev7.0_2021-10.pdf`
- DD-051, DD-052, DD-053, DD-059 in `DECISIONS.md`
