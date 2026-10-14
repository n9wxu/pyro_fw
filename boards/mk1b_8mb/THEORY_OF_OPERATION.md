# Pyro MK1B-8MB — Theory of Operation

An MK1B refitted with two parts (DD-097): U7 an 8 MB BYTe Semiconductor
BY25Q64ESHIG in place of the 2 MB W25Q16JV, and U5 the base AP2192
(AP2192MPG-13) in place of the AP2192A. Pins, sensor, buzzer, Lua and the
firing path are MK1B's, from `../mk1b/`, and `../mk1b/THEORY_OF_OPERATION.md`
describes them. This file covers what differs.

First built 2026-10-13: serial 0244102D6F98, 192.168.152.1, with an MS5607.

## What changes from MK1B

| | MK1B | MK1B-8MB |
|---|---|---|
| U7 | W25Q16JV, 2 MB | BY25Q64ESHIG, 8 MB |
| SDK board header | the stock `pico` | `sdk/pyro_mk1b_8mb.h`: `pico`, with 8 MB |
| littlefs | 984 kB, from 1064 kB | 4096 kB, from 4 MB |
| Application slots | 384 kB each | 1900 kB each |
| U5 | AP2192A: its output discharge holds the sense nodes at 0 V | AP2192: no discharge |
| Continuity | not judged; every channel reported ready | judged: igniter, open or short |

`board.cmake` includes MK1B's and changes the header and the geometry.
`CMakeLists.txt` builds MK1B's sources with this directory's headers first,
and defines `BOARD_PYRO_U5_DISCHARGES_OUTPUTS` 0 for them: `../mk1b/pyro_board.c`
includes the `board_pins.h` beside it, not this one.

## Flash

The BY25Q64ESHIG runs the stock W25Q080 boot stage 2: it reads status
register 2 with 35h, takes a two-byte 01h to set QE, keeps continuous EBh
reads with mode bits A0h, and gives its unique ID to 4Bh with four dummy
bytes (`docs/datasheets/BY25Q64ES_Rev2.9_2024-10-29.pdf`, pages 15, 29, 36
and 45). Its USON8 4x3 mm package fits MK1B's footprint (page 79). The first
board boots and runs from it.

Its unique ID begins with ASCII: the first board's is 35343239330594F3,
"5429" then four bytes. MK1C's XT25F128F ids share their ASCII part across
chips, and two MK1Cs took one identity from it. Here the MAC is drawn at
random (DD-072), so a shared id does not share an address, but
`support/register_board.py` reports one if it appears.

**An MK1B image and an MK1B-8MB image must not be swapped.** On a 2 MB chip
the 8 MB image's filesystem address, 4 MB, wraps to 0: the first boot finds
no filesystem there and formats over the bootloader (`hal_fs_mount()`). The
web page refuses the other board's file by name; the scripts in `support/`
pick images by the board the device reports. A hand-run `curl` or
`picotool load` checks nothing (task FS-1).

## Continuity check

As MK1B's, and now as written there: with no discharge the nodes read about
0 counts with an igniter, 4095 with nothing, and low on the short step with a
lead shorted. Good under 500 counts with no short, open over 3000 (DD-059).

Measured 2026-10-13, USB alone, nothing connected: 3451 and 3511 counts, open
on both channels, and the pad announces `check_pyro_1`. Why it reads below
4095 is not yet known; it is clear of the 3000 bound.

## What this board declares

The values the requirements leave to the board (BRD-01). Where a value is
MK1B's, it is because the refit does not touch what sets it. A value marked
*not measured* is owed to this board's HAL validation (BRD-02).

| Item | Declared |
|---|---|
| Pyro faults it can reliably detect | an overcurrent or overtemperature in U5, by its flag; an absent or open igniter; a lead shorted to ground. Open is measured; an igniter and a short are *not measured* on this board |
| `refire_interval` | MK1B's: default 1000 ms, 500 to 10000 ms (PYR-BOARD-01); not characterised |
| `fire_gap` | MK1B's: default 3000 ms, 1000 to 10000 ms; not characterised |
| Pulse | 500 ms, ended by the loop |
| Protection | MK1B's: U5's current limit and thermal shutdown, and F2, a 1.5 A PTC on the common. The PTC's reset time under a shorted match is *not measured* |
| Disarm when software stops (PYR-ARM-01) | **the watchdog, 1 s**, as MK1B: the loop ends the pulse. This exceeds the 50 ms bound |
| Sensor | MS5607 where fitted, else BMP280, found at start-up; ranges and heights as MK1B's. The first board's MS5607 on USB alone: 7.3 Pa rms (`support/pressure_trace.py`); *not measured* on a battery |
| Height for proper operation (SNS-MAX-01) | 30000 m with the MS5607; 9000 m with the BMP280 |
| Flight log (DAT-09) | 4096 kB of littlefs, 4,169,728 bytes free when blank: about 52 hours at `1hz`, 58 min at `full`. `/api/log/space` reports what is free |
| Delay of a flight decision (FLT-RT-01) | 250 ms declared. *Not measured*: `loop_late_max_us` on `/api/status` reports it |
| Script resources (LUA-PAD-03) | MK1B's: the J1 user pad, GPIO8, and a released pyro channel's pads (`../mk1b/pin_caps.h`) |
| Connector labels (PIN-LABEL-01) | `../mk1b/pin_caps.h` |

## Build

`cmake -B build-mk1b_8mb -DPYRO_BOARD=mk1b_8mb`. The images are
`pyro_fw_mk1b_8mb.uf2` and its bootloader, and `pyro_fw_c_fota_image.bin`
for the network. The release does not carry them (REL-1).

## Tests

- `board_pyro_tests` builds MK1B's `pyro_board.c` a second time with
  `BOARD_PYRO_U5_DISCHARGES_OUTPUTS` 0, as this board is built: igniter,
  empty, short and a bad joint judged against the netlist's node.
- `boards/sim_mk1b` with `plant_set_mk1b_u5()` runs the same file against
  the plant's base AP2192.
- The configure fails if `PYRO_FLASH_SIZE_KB` and the SDK header's
  `PICO_FLASH_SIZE_BYTES` disagree, since the bootloader's slots come from
  the header.
