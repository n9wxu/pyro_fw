# Hardware Porting Guide

Two kinds of port:

- **Another RP2040 board** is a directory under `boards/`. `boards/README.md`
  says what it holds, and `boards/reference/` is the template to copy, with a
  checklist in its `THEORY_OF_OPERATION.md`. Nothing outside the directory
  changes for it to build.
- **Another MCU family** is the rest of this document: what a port writes, what
  the flight software asks of the hardware, and what the RP2040 images use.

## What a port to another MCU writes

- **A complete `src/hal.h`.** `src/hal_common/` is written against the Pico SDK
  (`src/board_if.h` says so), so another family supplies its own HAL, as
  `boards/sim/hal_sim.c` does for the host in one file. Each pressure reading
  goes to `pp_feed_us()` with the time of its conversion, not of its read
  (SNS-PRES-08).
- **A board directory** with at least a `board_pins.h`: the flight software
  takes the board's name from it (`src/board_id.h`).
- **A board kind.** The top-level `CMakeLists.txt` branches on
  `PYRO_BOARD_KIND`, `pico` or `host`, never on a board's name. A new family is
  a new kind, which the top level has to learn; boards of that kind then need
  no edit to it.

Above `hal.h` nothing changes. The `sim` target builds that flight software
against `hal_sim.c` alone: `flight_states.c`, `brownout.c`, `pressure_processing.c`,
`pressure_fit.c`, `buzzer.c`, `beep_codes.c`, `beep_store.c`,
`telemetry_formatter.c`, `config.c`, `ground_test.c`, `ground_test_seq.c`,
`pad_claim.c` and `pyro_release.c`. The host suites run the same sources
against `test/hal_test.c`, whatever the target; a port's own proof is its HAL
on the bench.

## What the flight software asks of the hardware

| Need | What the RP2040 boards do | Decided in |
|---|---|---|
| A loop every 20 ms that never sleeps | `src/main_hardware.c`, period in `src/loop_period.h`; every settle is a deadline a later iteration checks | DD-053, DD-065 |
| At least 50 pressures a second, stamped at their conversions | The pressure collector: a timer interrupt whose handler runs from RAM commands, times and reads each conversion of either sensor, and recovers its own bus | SNS-PRES-08, SNS-COL-01..06, DD-093 |
| I2C as fast as the sensor and the PCB allow, every transfer bounded | 400 kHz where the pull-ups carry fast mode; the SDK's `_timeout_us` transfers, never the blocking ones | DD-052, DD-069 |
| Two pyro channels, each with a continuity sense | MK1A, MK1B: a switched high side per channel, one shared low side, a 100 kΩ pull-up on each sense node. MK1C: an eFuse armed by a charge pump, a low side per channel, bias injection and four ADC inputs | DD-055, DD-059 |
| A 12-bit ADC | the continuity thresholds are 12-bit counts: on MK1A and MK1B good under 500, open over 3000 | DD-059 |
| Firing outputs down before anything slow starts | `board_early_init()`, before USB, networking and the filesystem | — |
| A buzzer, an LED and a UART at 115200 | plain GPIO; the UART's transmit drained by an interrupt, its receive taking ground test commands | — |
| A ground test switch, optional | any one digital pad to ground, or two pads | DD-071 |
| Storage for config, scripts, web files and the flight log | littlefs on the program flash: 984 KB on MK1B, 8 MB on MK1A and MK1C. An erase or program stalls execute-in-place, so in flight only the log writes, and a conversion beside a flash operation is discarded | DD-058, DD-068 |
| A USB device | CDC-ECM/RNDIS networking under lwIP, serving the HTTP API and the web interface; picotool's reset interface | — |
| Updates in the field | two A/B slots behind `pico_fota_bootloader`, written over HTTP | — |
| A watchdog | twice the board's worst loop, `PYRO_LOOP_WORST_MS` | — |
| A second core, optional | Lua scripts on core1 (`PYRO_HAS_LUA`) | — |

Another medium for the flight log, such as an SD card, is designed but not
built: `docs/log_storage_options.md`.

## What the RP2040 images use

`arm-none-eabi-size` on the 2.1.678 images (2026-09-25), each with USB
networking, HTTP, littlefs and Lua:

| Board | Code and constants | RAM (data and bss) | Application slot |
|---|---|---|---|
| MK1A | 354 KB | 182 KB | 3948 KB |
| MK1B | 350 KB | 182 KB | 384 KB |
| MK1C | 358 KB | 186 KB | 3948 KB |

The RP2040 has 264 KB of RAM. A slot is (flash − littlefs − 40 KB) / 2 − 128 KB,
from the board's `board.cmake`; the top level prints it at configure. MK1B's is
the tight one, which is why the Lua interpreter is built for size.

## Other MCUs

DD-009 names the ESP32-C3 and the STM32C011 as further targets; neither
exists in the tree. The RP2350 and the ESP32-S3 are the candidates
`docs/log_storage_options.md` compares, with their datasheets in
`docs/datasheets/`.

> **The March 2026 study** -- candidate MCUs (SAMD21, STM32F072, PIC16F1455,
> ESP32-C3, STM32C011), product variants with HAL mappings, user-interface
> transports, and plans for a shared flight bus and a ground test plug -- is
> kept as written in `docs/porting_study_2026-03.md`. Its figures about this
> firmware are out of date; the ground test is by serial command (DD-011) and
> by switch (DD-071), not by plug.
