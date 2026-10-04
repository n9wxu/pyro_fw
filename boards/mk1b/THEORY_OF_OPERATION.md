# Pyro MK1B — Theory of Operation

How the MK1B flight computer works, from the pins up. The code refers here
instead of carrying the explanation itself; `support/trace_check.py` fails CI
if a comment's `See THEORY_OF_OPERATION.md "..."` names a heading this file
does not have.

The design is `~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch`; the pins and the
sense network below were read from its netlist
(`kicad-cli sch export netlist --format kicadsexpr`), as was the model in
`sim/plant/plant_mk1b.c`. Where a part differs, the table gives the part the
two JLCPCB builds' BOMs fit (`jlcpcbV1/`, `jlcpcbV2/` beside the schematic).

> **As built, MK1B cannot sense continuity.** Both JLCPCB builds fit U5 as the
> AP2192**A**, whose outputs discharge to ground while disabled and hold the
> sense nodes near 0 V. See [Known limits](#known-limits) and task B-U5.

## Overview

MK1B is a bare RP2040 that runs on the SDK's stock `pico` board definition. It
fires two igniters through a two-key circuit, the same topology as MK1A: a
switched high side per channel — the two halves of an AP2192 dual high-side
switch — and one shared low side. Current flows through an igniter only when
its high side **and** the shared low side are on.

The shared low side is also the continuity stimulus: with both high sides off,
switching it on lets a connected igniter pull its sense node to ground against
a weak pull-up. The AP2192 adds a fault flag per channel that MK1A lacks.

## Hardware

| Item | Part | Notes |
|---|---|---|
| MCU | U6 RP2040, QFN-56, 12 MHz crystal Y1 | bare chip on the SDK's own `pico` board header: GPIO25 is the LED here too |
| Flash | U7, a W25Q16JVUUIQ (2 MB, 3 V) by its marking, on a USON-8 4x3 mm footprint | 984 KB littlefs, the rest two OTA slots; see "Flash" below |
| High sides | U5 AP2192AMPG-13 (LCSC C507872) | EN1/EN2 gate OUT1/OUT2; FLG1/FLG2 open drain |
| Low side | Q1B, the second FET of an AO6800 | gate `sw_gnd` |
| Fuse | F2, 1.5 A PTC | resets |
| Sense | R26/R19 100 kΩ to 3V3; R25/R18 100 Ω; C25/C21 100 nF | per channel; D1 BAT54C clamps both nodes to 3V3 |
| Flag pull-ups | R21/R20 100 kΩ | |
| Pressure | U4 BMP280 (build V1) or U8 MS5607 (build V2), on separate SDA pads | a board carries one |
| Buzzer | BUZZER1 KXG0903C3, from VIN | switched on its low side by Q1A, the AO6800's first FET; R23 1 kΩ holds the gate low |
| LED | D3, blue, via R4 1 kΩ | |

## Flash

The chip on U7 is marked Q16JVUUIQ: a Winbond W25Q16JV, 16 Mbit, 2.7-3.6 V,
in the USON 4x3 mm package (`docs/datasheets/W25Q16JV_RevI_2024-12-24.pdf`,
page 70). It is powered from +3.3 V. Both builds' production files name
W25Q64JWUUIQ (LCSC C6604692) for U7 instead, a 1.7-1.95 V part that would be
out of its ratings on this rail; it is not what was fitted. The firmware
takes the chip as 2 MB (`PYRO_FLASH_SIZE_KB` in `board.cmake`) and the
SDK's `pico` header's W25Q080 boot stage 2.

A replacement has to suit the W25Q080 boot stage 2 as well as the
footprint. That boot stage reads status register 2 with 35h, sets its QE
bit (bit 1) with a two-byte 01h, and leaves the chip in continuous EBh
reads (mode bits A0h, M5-4 = 10); the SDK reads the unique ID, and with it
the board's name and addresses, with 4Bh and four dummy bytes.

- BYTe Semiconductor BY25Q64ESHIG(R), 8 MB, 2.7-3.6 V: does all four
  (`docs/datasheets/BY25Q64ES_Rev2.9_2024-10-29.pdf`, pages 15, 29, 36 and
  45), and its USON8 4x3 package matches the UU one (page 79). Not yet run
  on a board.
- GigaDevice GD25Q-E parts, among them the GD25Q32ENIGR (4 MB) and the
  GD25Q64ENIGR (8 MB): an 01h longer than one byte is not executed
  (GD25Q64E, section 7.4), so QE is never set. They need another boot stage
  2: the SDK's AT25SF128A one writes status register 2 with 31h.
- ISSI parts keep QE in status register 1, bit 6; the SDK's IS25LP080
  boot stage 2 exists for them.
- Winbond's W25Q32JV, the other 3 V part in the UU package, is no longer
  made.

No 16 MB 3 V part was found for this footprint.

The KiCad design as it now stands names U9, an XTX XT25F128FWOIGT-W
(LCSC C3202839: 16 MB, 2.7-3.6 V) on a WSON-8 6x5 mm footprint, in place of
U7. No board built from it has been seen.

## Pins

`board_pins.h` is the map, `pin_caps.h` says what each pin may become, and
`board_info.c` names the pyro pins for `picotool info -a`. `PICO_BOARD` is the
SDK's `pico`, so those pin names and the program name are what identify the
board.

| GPIO | Signal | Goes to |
|---|---|---|
| 0, 1 | UART0 TX, RX | J1.4, J1.5; R7/R6 4k7 pull-ups |
| 6 | I2C1 SDA | the BMP280 pad — no pull-up fitted |
| 7 | I2C1 SCL | both sensors, R10 4k7 |
| 8 | user pad | J1.6, Lua |
| 10 | I2C1 SDA | the MS5607 pad, R11 4k7 |
| 15 | PYRO_COMMON_EN | Q1B gate — the shared **low** side |
| 16 | buzzer | Q1A gate — the buzzer's low side |
| 17 | PYRO1_FLAG | U5 FLG2, active low |
| 18 | PYRO2_FLAG | U5 FLG1, active low |
| 21 | PYRO1_EN | U5 EN2 → OUT2 → channel 1 (CN1.1, drogue) |
| 22 | PYRO2_EN | U5 EN1 → OUT1 → channel 2 (CN1.4, main) |
| 25 | LED | R4 → D3 |
| 26, 27 | ADC0, ADC1 | SENSE1, SENSE2 |

The name PYRO_COMMON_EN reads as a high-side enable; the netlist puts it on
the low-side FET's gate. J1 also carries ground (J1.1), VBATT (J1.2) and the
input supply (J1.3).

## Start-up

`hal_platform_init()` silences the buzzer, then `board_early_init()` drives
PYRO_COMMON_EN and both enables low before USB, networking or the filesystem
start. `pyro_init()` repeats it, claims the sense inputs and pulls the two
flags up. The first continuity check starts from `pyro_update()`, never from
`pyro_init()`: a board with both channels released never calls
`pyro_update()`, so the common — by then Lua's pad — is never left raised.

## The main loop

The shared loop (`src/main_hardware.c`) runs every 20 ms (`src/loop_period.h`,
DD-065) and calls `pyro_update()` from its outputs stage. `pyro_update()`
either ends a fire pulse or advances the continuity check. Nothing waits
(DD-053).

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
| presence | on for its 10 ms settle, read at the next loop, 20 ms on | a channel pulled low has an igniter; one left high is open |

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
flight's post-fire verify window, which opens as the pulse ends and runs
100 ms. With the common on there is no short reading; the last one stands.

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
RP2040's own 50–80 kΩ (`docs/datasheets/rp2040-datasheet_2025-02-20.pdf`
page 617, Table 625), too slow an edge for fast mode, so its probe stays in
standard mode. The pressure collector then runs whichever was found, free
(DD-093): the MS5607 a pressure and a temperature every 18.8 ms, or the
BMP280 a forced conversion every 14.5 ms at its 100 kHz. Its handler runs
from RAM.

A transfer that fails is counted by cause on `/api/status`. After three in a
row the collector clears the bus on the fitted sensor's pad, resets the
sensor and goes on. A conversion a flash erase or program ran beside is
discarded, not used, and counted in `pres_flashed` on `/api/status` (DD-068).

## Telemetry, LED and buzzer

The defaults in `src/hal_common/board_defaults.c` drive all three: UART0 at
115200 on J1.4/J1.5, the LED lit from boot and toggled by the main loop's
pressure task every fifth reading, and the buzzer on GPIO16, which an operator
may move in `pins.ini`. GPIO16 drives only Q1A's gate; the buzzer's current
comes from VIN.

## Lua and released pads

Lua owns GPIO8. GPIO0/1 are not offered: `pin_caps.h` lists them only as the
UART, since moving it to Lua would cost the telemetry downlink. A pyro channel
can be released to Lua, and a released enable with a released common is a
half-bridge on PIO0, the pyro block. The F2 PTC and the AP2192's current limit
make a shoot-through trip and recover, which is why this board's protection
class is `PYRO_PROT_PTC_LIMITED`. With both channels released,
`hal_pyro_update()` stops calling `pyro_update()`.

## Ground test switch

A switch held closed at power-up puts the board in ground test mode, and
opening it fires the enabled channels on a countdown (DD-071). The one user
pad suits a switch to ground: GPIO8 (J1.6) to J1.1, `ground_test=ground` and
`ground_test_pin=8` in `pins.ini`. The pad then leaves Lua, which has no other.
The two-pad wiring needs a second digital pad, and on this board only a
released pyro pad is one.

## Faults

| Condition | What the board does |
|---|---|
| an overcurrent or overtemperature in U5 | its flag goes low; `pyro_fault()` is true |
| an igniter absent or open | reads open with the base AP2192; not judged as fitted — see [Known limits](#known-limits) |
| a lead shorted to ground | reads shorted with the base AP2192; not judged as fitted — see [Known limits](#known-limits) |

## Known limits

- **U5 is the AP2192A (task B-U5).** The A variant discharges each output to
  ground through about 100 Ω while it is disabled (DS32193 page 4, R_DIS, and
  its note 6). Against the 100 kΩ pull-ups that holds both sense nodes at a few
  millivolts whenever a channel is off — which is always, during a check. A
  fitted igniter, an empty connector and a real short all read the same, near
  0 counts, so the check cannot judge a channel. The board is built knowing
  that (`BOARD_PYRO_U5_DISCHARGES_OUTPUTS` in `board_pins.h`) and reports every
  channel ready, with the count it read: a board that cannot detect a fault
  treats the channel as ready, and no reading withholds a fire
  (PYR-HEALTH-01). The pad therefore says OK to fly whatever is connected. The
  base AP2192, AP2192MPG-13, has no discharge, the same
  pinout, active-high enables and MSOP-8EP drawing, and blocks reverse
  current, so the nodes read high when the battery is below 3.3 V
  (`docs/datasheets/AP2182_AP2192_DS31569_Rev10-2.pdf`, pages 1, 4 and 15);
  with it, and that define at 0, the check above works as written.
- **The BMP280 pad has no pull-up**, so a BMP280 board's sensor runs at
  100 kHz.
- **A beep code disturbs the MS5607 (task B-BZ).** On the bench, while one
  plays, the sensor's scatter rises from about 9 Pa to 31–38 Pa (DD-068). MK1C
  drives the same buzzer from VIN through its own AO3400A and shows none. On
  this board the buzzer's FET shares its AO6800 package with the pyro low
  side; the cause is not established.

## What this board declares

The values the requirements leave to the board (BRD-01). A value marked
*not measured* is owed to this board's HAL validation (BRD-02).

| Item | Declared |
|---|---|
| Pyro faults it can reliably detect | an overcurrent or overtemperature in U5, by its flag. Open and shorted are **not** reliably detected while U5 is the AP2192A, whose output discharge holds the sense nodes at 0 V (see [Known limits](#known-limits)): a channel it cannot judge is treated as ready (PYR-HEALTH-01) |
| `refire_interval` | default 1000 ms, 500 to 10000 ms: the general values (PYR-BOARD-01); this board has not been characterised for its own |
| `fire_gap` | default 3000 ms, 1000 to 10000 ms: the general values; not characterised |
| Pulse | 500 ms, ended by the loop |
| Protection | U5's current limit and thermal shutdown, which recover by themselves, and F2, a 1.5 A PTC on the common, which resets as it cools. The PTC's reset time under a shorted match is *not measured*, and is what this board's `fire_gap` should come from |
| Disarm when software stops (PYR-ARM-01) | **the watchdog, 1 s.** The loop ends the pulse, so a stopped loop leaves the enable on until the reset. This exceeds the 50 ms bound |
| Sensor | MS5607 where fitted, else BMP280, found at start-up. MS5607: 10 to 1200 mbar, 2.4 Pa rms at OSR 4096 (`docs/datasheets/MS5607-02BA03_2017-06.pdf`, pages 1 and 4). Measured on this board: 9 Pa, rising to 35 Pa while the buzzer sounds or the flash is written |
| Height for proper operation (SNS-MAX-01) | 30000 m with the MS5607; 9000 m with the BMP280 |
| Flight log (DAT-09) | 984 kB of littlefs: about 12 days at `1hz`, 15 min at `full`. `/api/log/space` reports what is free |
| Delay of a flight decision (FLT-RT-01) | 250 ms declared. *Not measured* on this revision: `loop_late_max_us` on `/api/status` reports it |
| Script resources (LUA-PAD-03) | the J1 user pad, GPIO8, and a released pyro channel's pads (`pin_caps.h`) |
| Connector labels (PIN-LABEL-01) | `pin_caps.h` |

The disarm time is this board's hardware: nothing but the processor ends a
pulse. MK1C's charge pump is what meets PYR-ARM-01.

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
- `sensor_bringup_tests`, `collector_tests` and `ms5607_tests` cover the
  two-pad bring-up, a BMP280 holding the bus (DD-069), the collector and the
  arithmetic.
- `integration_tests` flies the flight software built for MK1B, the default
  board.
- `plant_tests` models the sense network from the netlist; it leaves out U5's
  discharge.
- `boards/sim_mk1b` runs the real `pyro_board.c` against the plant.

## References

- `~/Documents/pyro_mk1b/pyro_mk1b.kicad_sch`, and the JLCPCB BOMs beside it
- Diodes DS32193 (AP2182A/AP2192A) and DS31569 (AP2182/AP2192), from
  diodes.com
- `docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`, `MS5607-02BA03_2017-06.pdf`,
  `UM10204_I2C-bus_Rev7.0_2021-10.pdf`
- DD-052, DD-053, DD-059, DD-065, DD-068, DD-069, DD-093,
  DD-071 in `DECISIONS.md`
- Tasks B-U5 and B-BZ in `docs/outstanding_tasks.md`, section 6
