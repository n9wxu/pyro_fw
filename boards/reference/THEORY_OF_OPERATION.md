# Reference board — Theory of Operation

This directory is not hardware. It is the template a new board is copied
from, and this file is two things: how the template itself behaves, and the
outline a real board's `THEORY_OF_OPERATION.md` fills in. Replace it when you
copy the directory. `boards/mk1c/THEORY_OF_OPERATION.md` is the most complete
worked example.

## Overview

As shipped the template compiles and links, and it is safe: it reports no
continuity, refuses every fire, and drives nothing. A board brought up from it
energises nothing until its real pyro backend is written. It assumes an RP2040
(`PYRO_BOARD_KIND pico`), one pressure sensor on one I2C bus, and a plain-GPIO
LED, UART and buzzer.

## Porting steps

```bash
cp -r boards/reference boards/<name>
$EDITOR boards/<name>/board_pins.h      # every TODO, against the netlist
$EDITOR boards/<name>/pin_caps.h        # every TODO
$EDITOR boards/<name>/board.cmake       # SDK header, flash geometry, Lua, loop budget
cmake -B build-<name> -DPYRO_BOARD=<name> && cmake --build build-<name>
```

Nothing outside the board's directory changes. Then write the pyro backend,
its host tests, and this file.

## Files

| File | Holds | Required |
|---|---|---|
| `board_pins.h` | identity, capabilities, the pin map, sensor bus speeds | yes |
| `pin_caps.h` | what each pin may become; the pyro topology and protection class | yes |
| `pyro_board.c` | `src/pyro.h` and `board_early_init()` | yes |
| `board_info.c` | the pins `picotool info -a` names | recommended |
| `board.cmake` | settings the Pico SDK needs before it initialises | yes |
| `CMakeLists.txt` | the `pyro_board` library | yes |
| `sdk/<name>.h` | a Pico SDK board header, for a bare RP2040 | if not a Pico module |
| `pressure_board.c` | `src/pressure_sensor.h`, for two sensors on one bus | only then |

What a board need not write:

- **LED, UART, buzzer and `board_pyro_raw()`.** `src/hal_common/board_defaults.c`
  drives them as plain GPIO on the pins `board_pins.h` names. Each is weak: a
  board with other hardware behind one — a piezo driver, a PWM slice —
  defines its own and that one links.
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
  number shows it. The flight gates firing on good, a second barrier behind
  whatever `pyro_fire()` checks.
- **Never wait.** `pyro_update()` runs every loop; a settle is a deadline a
  later call checks (`deadline_reached()` in `src/board_support.h`), never a
  sleep. `support/wait_check.py` fails CI on any sleep in `boards/`.
- **Answer `pyro_is_firing()` straight after `pyro_fire()`**: that is the
  flight's acknowledgement, and a board that refuses leaves it false.
- **Check only presence and shorts** between fires (DD-055).

Name the pins from the netlist, not from a schematic image: on MK1C, GPIO25 is
a pyro bias injector, where MK1A and MK1B put their LED.

## Pressure sensor

Declare the sensor by its bus speed, as fast as the device allows and no faster
than the PCB carries: fast mode's 300 ns rise needs pull-ups of at most
300 ns / (0.8473 × Cb), 4k7 to about 75 pF (UM10204 pages 44 and 50), and the
RP2040's own 50–80 kΩ are too weak for it (DD-052). Read the pull-ups from the
design files. The template declares 100 kHz until that is checked.

## Build

- **A bare RP2040 needs its own SDK header.** It must carry
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
against the plant.
