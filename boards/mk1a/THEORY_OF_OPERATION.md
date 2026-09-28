# Pyro MK1A — Theory of Operation

How the MK1A flight computer works, from the pins up. The code refers here
instead of carrying the explanation itself; `support/trace_check.py` fails CI
if a comment's `See THEORY_OF_OPERATION.md "..."` names a heading this file
does not have.

The design is the schematic `~/Documents/Pyro_mk1a.pdf`. There is no KiCad
source for this board to export a netlist from (`~/Documents/pyro/` is a
different design); every GPIO here was checked against the schematic on
2026-09-26.

## Overview

MK1A fires two igniters through a two-key circuit: each channel has its own
switched high side, and both share one switched low side. Current flows through
an igniter only when its high side **and** the shared low side are on, so either
one off breaks the circuit.

The same low side is the continuity stimulus. With both high sides off,
switching the low side on lets a connected igniter pull its sense node to
ground against a weak pull-up — continuity is read with no current from the
battery at all.

## Hardware

| Item | Part | Notes |
|---|---|---|
| MCU | RP2040, QFN-56, 12 MHz crystal Y1 | bare chip; its own SDK header, `sdk/pyro_mk1a.h` |
| Flash | W25Q128JVS, 16 MB | 8 MB littlefs, the rest two OTA slots |
| High sides | Q6, Q1 DMC2053UVT dual MOSFETs | one per channel; 1 kΩ gate pull-downs |
| Low side | Q2 AO3400A | shared; 1 kΩ gate pull-down |
| Fuse | F1, 8 A | does not reset |
| Sense | R9/R10 100 kΩ to 3V3; R5/R14 1 kΩ; C6/C5 100 nF | per channel |
| Pressure | U4 BMP280 | the only sensor fitted |
| LED | D3 via R12 1 kΩ; D4 (R13) is the 3V3 power light | |

No buzzer is fitted, and no part on the pyro path has a fault output.

## Pins

`board_pins.h` is the map, `pin_caps.h` says what each pin may become, and
`board_info.c` names the pyro pins for `picotool info -a`.

| GPIO | Signal | Goes to |
|---|---|---|
| 0 | UART0 TX | D7 1N4148 → the J6.3 node |
| 1 | UART0 RX | the J6.3 node |
| 9 | FIRE1 | Q6A gate — channel 1 high side, J3 (drogue) |
| 10 | PYRO_LOW | Q2 gate — the shared low side |
| 11 | FIRE2 | Q1A gate — channel 2 high side, J4 (main) |
| 18, 19 | user pads | J6 header, Lua |
| 20, 21 | I2C0 SDA, SCL | BMP280, R1/R2 4k7 pull-ups |
| 25 | LED | R12 → D3 |
| 26, 27 | ADC0, ADC1 | SENSE1, SENSE2 |

GPIO18/19 are also I2C1's pads, but their pull-ups R18/R20 are not fitted and
nothing on I2C1 is. GPIO25 really is the LED here, so the SDK header may name it
`PICO_DEFAULT_LED_PIN`; the header exists for the flash size, since the stock
`boards/pico.h` declares 2 MB and would give the bootloader a wrong slot map.

## Start-up

`hal_platform_init()` silences the buzzer (none is fitted, so nothing happens),
then `board_early_init()` drives FIRE1, FIRE2 and PYRO_LOW low before USB,
networking or the filesystem start. The pads' reset state already holds them
there; this makes it explicit. `pyro_init()` repeats it, claims the two sense
inputs, and starts the first continuity check.

## The main loop

The shared loop (`src/main_hardware.c`) runs every 20 ms (`src/loop_period.h`, DD-065) and calls
`pyro_update()` from its outputs stage. `pyro_update()` either ends a fire pulse
or advances the continuity check by one step. Nothing waits (DD-053): every
settle is a deadline a later iteration checks.

## Pyro circuit

```
VBATT ── Q6 [FIRE1] ── J3 igniter ─┐
VBATT ── Q1 [FIRE2] ── J4 igniter ─┴── Initiator_ground ── F1 8A ── Q2 [PYRO_LOW] ── GND

         +3V3 ── R9 100k ──┬── J3 high node ── R5 1k ──┬── ADC0
                           │                         C6 100n
         +3V3 ── R10 100k ─┴── J4 high node ── R14 1k ─┬── ADC1
                                                      C5 100n
```

`pyro_fire()` is the only function that raises FIRE1 or FIRE2, and only with
PYRO_LOW. No check, self-test or boot path raises either, which is what makes the
continuity check safe to run all the time.

## Continuity check

Every 500 ms, in three steps, one per deadline:

| Step | PYRO_LOW | Lasts | Reads |
|---|---|---|---|
| presence | on | 50 ms | a channel pulled low has an igniter; one left high is open |
| shorts | off | 50 ms | a channel still low has a path to ground bypassing the low side: a short |
| idle | off | 400 ms | — |

Against the 100 kΩ pull-up, at 12 bits:

| Path to ground | Counts | Verdict |
|---|---|---|
| a 2 Ω igniter | 0 | good |
| a 1 kΩ bad joint | 41 | good, and the count shows it |
| a 10 kΩ leak | 372 | good, and the count shows it |
| nothing | 4095 | open |

A channel is good under 500 counts with no short, open over 3000, and in
between it is neither — a degraded joint lands there instead of being rounded to
good, and `pyro_get()` reports the raw count so the number shows it. Until the
first check completes, every channel reads open.

**The settle is the slow edge.** Going low is fast (1 kΩ into 100 nF,
100 µs), but reading open means charging C6/C5 through 101 kΩ, a 10.1 ms
constant; 50 ms is five of them. That cannot be a wait inside a 20 ms loop, so
each step parks on a deadline. Holding the low side on only 50 ms in 500 keeps
the exposure small if a high side ever leaks.

## Firing

`pyro_fire()` turns the low side and the channel's high side on and starts a
500 ms pulse; the check is suspended, since the pulse owns the low side. When
the pulse ends, the high side goes off and a new check starts **at once**, with
the low side still on. The flight's post-fire verify window opens as the pulse
ends and looks for the channel gone open; resuming the check anywhere else
would leave the pre-fire reading latched through that window, and a channel
still reading good there counts as a failed verify. Starting at once lands a
fresh reading about 50 ms in.

## Pressure sensor

One BMP280 on I2C0 at 400 kHz, its fastest: R1 and R2 (4k7) hold fast mode's
300 ns rise to about 75 pF (DD-052). It runs in normal mode, converting on its
own every 11.5 ms (pressure ×4, temperature ×1), and the flight reads it every
20 ms. Bring-up is the shared single-sensor path,
`src/pressure_single_sensor.c`: clock the bus free — the sensor stays powered
across a CPU reset and can be left holding SDA low — hand it to the I2C block,
let the pull-ups settle, detect.

## Telemetry, LED and buzzer

The telemetry UART reaches the outside on one wire, J6.3. TX and RX meet at one
node, pulled up by R17 1 kΩ and taken to J6.3 through R19 1 kΩ; TX can only
pull the node low, through D7. The line is half duplex, and RX hears everything
TX sends. J6.4 is the switched supply and J6.5 ground.

The defaults in `src/hal_common/board_defaults.c` drive the UART pins and the
LED, lit from boot and toggled from the main loop. No buzzer is fitted and
`board_pins.h` names no buzzer pin, so the buzzer starts on no pad; an operator
can give MK1A one by assigning a pad in `pins.ini`.

## Lua and released pads

Lua owns the two J6 user pads (GPIO18/19). J6.3 is not offered: it echoes back
everything sent on it. A pyro channel can be released to Lua, and a released
FIRE with a released PYRO_LOW is a half-bridge whose midpoint is the shorted
igniter terminals; it runs on PIO0, the pyro block. F1 is 8 A and does not
reset, which is why this board's protection class (`PYRO_PROT_FUSE_ONESHOT`)
differs from MK1B's.

## Faults

| Condition | What the board does |
|---|---|
| an igniter absent or open | reads open |
| a lead or connector shorted to ground | reads shorted |
| a degraded joint | reads good, or neither, with its raw count |
| an overcurrent | nothing to report: `pyro_fault()` is always false |

## Known limits

- **During a pulse the sense pins see the pack.** The high side puts VBATT on
  the igniter's high node, which SENSE1/2 tap through 1 kΩ: on 2S about 5 mA
  into the RP2040's ADC clamp for the 500 ms of the pulse. It is survivable, and
  firmware cannot change it.
- **No fault output**, so an overcurrent is invisible to the firmware.
- **No host test runs this board's pyro backend** in CI; `boards/sim_mk1a` runs
  it against the plant model.

## Build

`board.cmake` selects the SDK header, the 16 MB geometry and Lua, and declares
the loop budget, `PYRO_LOOP_WORST_MS` (500 ms, dominated by a flash sector
erase). `/api/status` reports `loop_max_us` and `loop_overruns`.

| File | Holds |
|---|---|
| `pyro_board.c` | `src/pyro.h` and `board_early_init()` |
| `board_info.c` | picotool's pin names |
| `src/pressure_single_sensor.c` | the sensor bring-up, shared |

## Tests and models

- `plant_tests` models this board's sense network (`sim/plant/plant_mk1a.c`):
  the counts above and how long a just-opened channel takes to read open.
- `sensor_bringup_tests` runs the bring-up against a fake BMP280.
- `integration_tests` flies the flight software built for MK1A.
- `boards/sim_mk1a` runs the real `pyro_board.c` against the plant.

## References

- `~/Documents/Pyro_mk1a.pdf`, the schematic
- `docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`,
  `rp2040-datasheet_2025-02-20.pdf`, `UM10204_I2C-bus_Rev7.0_2021-10.pdf`
- DD-052 (bus speeds), DD-053 (no waits), DD-059 (MK1B reads as MK1A does) in
  `DECISIONS.md`
