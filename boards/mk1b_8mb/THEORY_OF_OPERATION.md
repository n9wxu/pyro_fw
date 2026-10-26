# Pyro MK1B-8MB — Theory of Operation

An MK1B refitted with two parts (DD-097): U7 an 8 MB BYTe Semiconductor
BY25Q64ESHIG in place of the 2 MB W25Q16JV, and U5 the base AP2192
(AP2192MPG-13) in place of the AP2192A. Pins, sensor, buzzer, Lua and the
firing path are MK1B's, from `../mk1b/`, and `../mk1b/THEORY_OF_OPERATION.md`
describes them. This file covers what differs. Two boards are built.

## What changes from MK1B

| | MK1B | MK1B-8MB |
|---|---|---|
| U7 | W25Q16JV, 2 MB | BY25Q64ESHIG, 8 MB |
| SDK board header | the stock `pico` | `sdk/pyro_mk1b_8mb.h`: `pico`, with 8 MB |
| Files (alfs, DD-100), at the top of flash | 984 kB, from 1064 kB | 4096 kB, from 4 MB |
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
and 45). Its USON8 4x3 mm package fits MK1B's footprint (page 79). Both
boards boot and run from it.

Its datasheet suspends an erase (page 62), but an erase resumed and
suspended in each 3 ms slot of the flight's log writer does not end within
2,000 slots, and a chip holding an erase suspended ignores every later erase
until it is resumed or powered off. So this board does not erase in flight
(DD-104, FLT-LOG-10, `src/flash_timing.c`): its log has the room the pad's
cleanup erased before launch. Its program cannot be suspended, and starts
in a slot's first 300 us.

Its unique ID is 128 bits (page 45). The SDK reads the first 64, the half two
chips can share (DD-098). `/api/status` reports all 128 as `flash_uid`, and
the MAC and subnet are derived from all 128 (DD-099).

**An MK1B image and an MK1B-8MB image must not be swapped.** On a 2 MB chip
the 8 MB image's file area, from 4 MB, wraps to 0, onto the bootloader, which
the store's cleanup would then erase. The web page refuses the other board's
file by name; the scripts in `support/` pick images by the board the device
reports. And the image itself reads the chip's size from its JEDEC ID at
boot (DD-098): on a chip smaller than its flash map it mounts nothing
(`hal_fs_mount()`), so its cleanup erases nothing, takes no update, reports
`board_mismatch`, and after an update rolls back.

## Continuity check

MK1B's check, which with no discharge works as written there: the nodes
read about 0 counts with an igniter, 4095 with nothing, and low on the short
step with a lead shorted. Good under 500 counts with no short, open over 3000
(DD-059). After a fire the channel has its verdict from the fresh reading,
as MK1B's "Firing" describes.

Measured 2026-10-13, USB alone, nothing connected: 3451 and 3511 counts on
the first board, 3981 and 3970 on the second, open on both channels, and the
pad announces `check_pyro_1`. Why it reads below 4095 is not known; it is
clear of the 3000 bound.

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
| Height for proper operation (SNS-MAX-01) | 9144 m (30,000 ft, the design's ceiling, DD-111) with the MS5607; 9000 m, the sensor's 300 hPa, with the BMP280 |
| Flight log (DAT-09) | 4096 kB of files, 16,383 pages, 3,866,388 bytes free when blank: about 39 minutes of flight. Every sample is a 22-byte row, up to 69 a second from a BMP280 (53 from an MS5607), beside a row a second for each estimator; each second's sync stores its partial page [FLT-LOG-06], so a second takes about seven pages. `/api/log/space` reports the seconds the next flight has |
| Delay of a flight decision (FLT-RT-01) | 250 ms declared. *Not measured*: `loop_late_max_us` on `/api/status` reports it |
| Script resources (LUA-PAD-03) | MK1B's: the J1 user pad, GPIO8, and a released pyro channel's pads (`../mk1b/pin_caps.h`) |
| Connector labels (PIN-LABEL-01) | `../mk1b/pin_caps.h` |

## Build

`cmake -B build-mk1b_8mb -DPYRO_BOARD=mk1b_8mb`. The images are
`pyro_fw_mk1b_8mb.uf2` and its bootloader, and `pyro_fw_c_fota_image.bin`
for the network. Releases carry them as `fw_mk1b_8mb.uf2`,
`fw_mk1b_8mb_bootloader.uf2` and `fw_mk1b_8mb_fota.bin`. It is built with
the bench flight (`PYRO_HAS_BENCH_FLIGHT`, DD-078): `support/bench_flight.py`
flies it.

## Tests

- `board_pyro_tests` builds MK1B's `pyro_board.c` a second time with
  `BOARD_PYRO_U5_DISCHARGES_OUTPUTS` 0, as this board is built: igniter,
  empty, short and a bad joint judged against the netlist's node.
- `plant_tests` runs the plant's MK1B with the base AP2192
  (`plant_set_mk1b_u5()`): a fitted igniter near 0 counts, an empty
  connector 4095.
- The configure fails if `PYRO_FLASH_SIZE_KB` and the SDK header's
  `PICO_FLASH_SIZE_BYTES` disagree, since the bootloader's slots come from
  the header.
