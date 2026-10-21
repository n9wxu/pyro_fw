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
against `hal_sim.c` alone: the files `src/flight_sources.txt` lists, with
`buzzer.c`, `flight_log.c`, `pad_claim.c` and `pyro_release.c`. The host
suites run the same sources against `test/hal_test.c`, whatever the target; a
port's own proof is its HAL on the bench.

## What the flight software asks of the hardware

| Need | What the RP2040 boards do | Decided in |
|---|---|---|
| A loop every 10 ms that never sleeps | the flight task in `src/main_hardware.c`, woken by a hardware-timer alarm, period in `src/loop_period.h`; every settle is a deadline a later iteration checks | DD-053, DD-104 |
| At least 50 pressures a second, stamped at their conversions | The pressure collector: a timer interrupt whose handler runs from RAM commands, times and reads each conversion of either sensor, and recovers its own bus | SNS-PRES-08, SNS-COL-01..06, DD-093 |
| I2C as fast as the sensor and the PCB allow, every transfer bounded | 400 kHz where the pull-ups carry fast mode; the collector queues each transfer whole and reads its outcome at its next alarm, and detection uses the SDK's `_timeout_us` transfers, never the blocking ones | DD-052, DD-069, DD-093 |
| Two pyro channels, each with a continuity sense | MK1A, MK1B: a switched high side per channel, one shared low side, a 100 kΩ pull-up on each sense node. MK1C: an eFuse armed by a charge pump, a low side per channel, bias injection and four ADC inputs | DD-055, DD-059 |
| A 12-bit ADC | the continuity thresholds are 12-bit counts: on MK1A and MK1B good under 500, open over 3000 | DD-059 |
| Firing outputs down before anything slow starts | `board_early_init()`, before USB, networking and the filesystem | — |
| A buzzer, an LED and a UART at 115200 | plain GPIO; the UART's transmit drained by an interrupt. The port takes no commands | TEL-12 |
| A ground test switch, optional | any one digital pad to ground, or two pads | DD-071, DD-087 |
| Storage for config, scripts, web files and the flight log | alfs on the program flash: 984 KB on MK1B, 4 MB on MK1B-8MB, 8 MB on MK1A and MK1C. An erase or program stalls execute-in-place, so in flight only the log writes, into erased pages, while core0 runs from RAM; a conversion beside a flash operation is used and counted. MK1C-SD puts every file but the board's identity and pad record on an SD card while one is mounted, with a high-rate log of the IMU and every conversion | DD-058, DD-076, DD-077, DD-100, DD-104, DD-106 |
| A USB device | CDC-ECM/RNDIS networking under smallest_tcp, serving the HTTP API and the web interface; picotool's reset interface | DD-102 |
| Updates in the field | two A/B slots behind `pico_fota_bootloader`, written over HTTP | — |
| A watchdog | twice the board's worst loop, `PYRO_LOOP_WORST_MS` | — |
| Two cores | FreeRTOS SMP: the flight task alone on core0; the network, the log writer and Lua on core1, the writer and Lua in each period's frame | DD-073, DD-104 |

## What the RP2040 images use

`arm-none-eabi-size -A` on the 2.3.0 images, built with arm-none-eabi-gcc
13.2 (2026-10-20), each with USB networking, HTTP, the filesystem and Lua.
The image includes the flight path, which start-up copies to RAM (DD-104):

| Board | Image in flash | RAM (data and bss) | Application slot |
|---|---|---|---|
| MK1A | 321 KB | 246 KB | 3948 KB |
| MK1B | 322 KB | 248 KB | 384 KB |
| MK1B-8MB | 335 KB | 249 KB | 1900 KB |
| MK1C | 336 KB | 250 KB | 3948 KB |

The RP2040 has 264 KB of RAM: 256 KB in the main banks, which data and bss
share with the heap, and two 4 KB banks. A slot is (flash − files − 40 KB) / 2 − 128 KB, from the
board's `board.cmake`; the top level prints it at configure. MK1B's is the
tight one, which is why the Lua interpreter, FreeRTOS, smallest_tcp and
TinyUSB are built for size.

## Other MCUs

No other MCU is in the tree. The RP2350's and the ESP32-S3's datasheets are in
`docs/datasheets/`.
