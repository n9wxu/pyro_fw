# Reference board — Theory of Operation

This directory is not hardware. It is the template a new board is copied
from, and this file is two things: how the template itself behaves, and the
outline a real board's `THEORY_OF_OPERATION.md` fills in. Replace it when you
copy the directory. `boards/mk1c/THEORY_OF_OPERATION.md` is the most complete
worked example.

## Overview

As shipped the template compiles and links, and it is safe: it reports no
continuity, has no firing path, and drives nothing. A board brought up from it
energises nothing until its real pyro backend is written. It assumes an RP2040
(`PYRO_BOARD_KIND pico`), one pressure sensor on one I2C bus, and a plain-GPIO
LED, UART and buzzer, and it builds without Lua. CI builds it as shipped
(`.github/workflows/build.yml`).

## Porting steps

```bash
cp -r boards/reference boards/<name>
$EDITOR boards/<name>/board_pins.h      # every TODO, against the netlist
$EDITOR boards/<name>/pin_caps.h        # every TODO
$EDITOR boards/<name>/board.cmake       # SDK header, flash geometry, Lua, loop budget
cmake -B build-<name> -DPYRO_BOARD=<name> && cmake --build build-<name>
```

Nothing outside the board's directory changes for it to build. Then write the
pyro backend, its host tests, and this file.

Lua takes three edits, as `boards/mk1a` has them: `set(PYRO_HAS_LUA 1)` in
`board.cmake`, the `if(PYRO_HAS_LUA)` block that adds the Lua PIO sources to
`CMakeLists.txt`, and the pads in `pin_caps.h`'s `LUA_PIN_LIST` and
`LUA_PIN_COUNT`, each with a row offering Lua functions.

## Files

| File | Holds | Required |
|---|---|---|
| `board_pins.h` | identity, capabilities, the pin map, sensor bus speeds | yes |
| `pin_caps.h` | what each pin may become; the pyro topology and protection class | yes |
| `pyro_board.c` | `src/pyro.h` and `board_early_init()` | yes |
| `board_info.c` | the pins `picotool info -a` names | recommended |
| `board.cmake` | settings the Pico SDK needs before it initialises | yes |
| `CMakeLists.txt` | the `pyro_board` library | yes |
| `sdk/<name>.h` | a Pico SDK board header | if the stock `pico` header does not describe the board |
| `pressure_board.c` | `src/pressure_sensor.h`, for two sensors on one bus | only then |

What a board need not write:

- **LED, UART, buzzer and `board_pyro_raw()`.** `src/hal_common/board_defaults.c`
  drives them as plain GPIO on the pins `board_pins.h` names. Each is weak: a
  board with other hardware behind one — a piezo driver, a PWM slice —
  defines its own and that one links.
- **`board_flash_ok()`**, weak and always true in `src/flash_window.c`. A
  board whose firing sequence is paced by the loop answers false while it
  runs, as MK1C does: a flash erase stalls the loop (DD-056).
- **The ground test switch.** `src/hal_common/` reads it on whichever pads
  `pins.ini` names; any row with `FN_DIGITAL` can carry it, a pyro pad once
  its channel is released (DD-071).
- **The pressure sensor's bring-up**, for one sensor on one bus:
  `src/pressure_single_sensor.c` picks the sensor from whichever bus speed
  `board_pins.h` declares, `BOARD_MS5607_I2C_HZ` or `BOARD_BMP280_I2C_HZ`.
- **Everything else** — the UART's interrupt-driven ring, littlefs, config,
  flight logging, USB, networking — lives in `src/hal_common/` and `src/`.

## Pyro backend

`src/pyro.h` is a frozen contract, and so is `src/hal.h`: if a board seems to
need a new function there, the behaviour probably belongs in the flight layer,
which has the flight state. A backend that can fire must:

- **Put every pyro output down in `board_early_init()`**, which runs before
  USB, networking or the filesystem, and again in `pyro_init()`.
- **Raise a firing output in one place.** One function, reviewable as a unit;
  no check, self-test or boot path raises one.
- **Report good only when continuity is proven**, with the raw count beside
  the booleans: a degraded match sits between the thresholds, and only the
  number shows it. Good is for the pad's announcement.
- **Withhold no fire.** `pyro_fire()` delivers the pulse whatever the board
  has measured: no health reading, latched fault or low pack stops it
  (PYR-FIRE-01, PYR-HEALTH-01). Protection may end a pulse; it is cleared
  before the next (PYR-FAULT-01). A firing path that needs preparing fires
  when it is ready or at its deadline (PYR-ARM-03).
- **Never wait.** `pyro_update()` runs every loop; a settle is a deadline a
  later call checks (`deadline_reached()` in `src/board_support.h`), never a
  sleep. `support/wait_check.py` fails CI on any sleep in `boards/`, and on
  the SDK's blocking I2C transfers: use the `_timeout_us` forms (DD-069).
- **Answer `pyro_is_firing()` straight after `pyro_fire()`**: that is what
  the board observed of the pulse, which the flight records. False means the
  pulse energised nothing.
- **Check only presence and shorts** between fires (DD-055).

Name the pins from the netlist, not from a schematic image: on MK1C, GPIO25 is
a pyro bias injector, where MK1A and MK1B put their LED.

## Pressure sensor

Declare the sensor by its bus speed, as fast as the device allows and no faster
than the PCB carries: fast mode's 300 ns rise needs pull-ups of at most
300 ns / (0.8473 × Cb), 4k7 to about 75 pF (UM10204 pages 44 and 50), and the
RP2040's own 50–80 kΩ (RP2040 datasheet page 617, Table 625) are too weak for
it (DD-052). Read the pull-ups from the design files. The template declares
100 kHz until that is checked.

An MS5607 wants fast mode: the one-shot converts a pressure and a temperature
every 20 ms loop, and at 100 kHz the pair is ready only 0.26 ms before the
next, short of the 0.5 ms `ms5607_tests` asks (DD-066). A BMP280 runs one
forced conversion a loop at either speed (DD-067).

## What this board declares

The values the requirements leave to the board (BRD-01). A value marked
*not measured* is owed to this board's HAL validation (BRD-02).

| Item | Declared |
|---|---|
| Pyro faults it can reliably detect | none: the stub reports every channel open |
| `refire_interval` | default 1000 ms, 500 to 10000 ms: the general values (PYR-BOARD-01); declare the board's own in `board_pyro_limits()` |
| `fire_gap` | default 3000 ms, 1000 to 10000 ms: the general values |
| Pulse, protection, disarm | none: the stub has no firing path. A real backend declares each |
| Sensor and height | the sensor `board_pins.h` gives a bus speed for (`src/pressure_sensor.h` has each part's range) |
| Flight log (DAT-09) | `PYRO_PFB_FS_KB` in `board.cmake` |
| Delay of a flight decision (FLT-RT-01) | 250 ms, to be measured on the board |
| Script resources (LUA-PAD-03) | none as shipped: `pin_caps.h` |
| Connector labels (PIN-LABEL-01) | `pin_caps.h` |

A board copied from this template replaces every row with its own.

## Build

- **The SDK board header.** The stock `pico` header declares 2 MB of flash and
  an LED on GPIO25, and leaves the crystal at the SDK's default 12 MHz; MK1B
  fits it, bare RP2040 as it is.
  MK1A has its own for its 16 MB, MK1C for its 16 MB and because its GPIO25 is
  not an LED. An own header must carry
  `pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, ...)` as well as the
  `#define`: without the directive the bootloader's linker script gets a
  negative flash size that still links. And it must not name
  `PICO_DEFAULT_LED_PIN` unless that pin really is an LED, because TinyUSB's
  BSP drives it as an output (`boards/mk1c/sdk/pyro_mk1c.h`).
- **`PYRO_PFB_FS_KB`** is the littlefs reservation; the top level checks the
  A/B slot arithmetic and fails the configure with it shown.
- **`PYRO_LOOP_WORST_MS`** is the loop's worst case; the watchdog is twice it.
  Measure `loop_max_us` on `/api/status` under the worst load you can provoke —
  a log flush during an OTA upload — and set it to that, rounded up.

## Tests

A board's backend runs on the host against a stand-in SDK: `test/fake_sdk` for
a pin-level test (`board_pyro_tests` does MK1B), or `sim/hw/` with a plant model
in `sim/plant/` for one that needs the circuit's physics
(`board_pyro_mk1c_tests`). `sensor_bringup_tests` builds once per board. Add the
new board to both, and a `boards/sim_<name>` package to run the flight software
against the plant. `ms5607_tests` and `integration_tests` build against the
selected board's package, so run them in the new board's build directory, and
add the board to `.github/workflows/build.yml`.
