# Pyro MK1C-SD — Theory of Operation

MK1C with its J3 header and J1.6 given to an SPI bus: an SD card and an
LSM6DS3 accelerometer and gyroscope (DD-075). Everything else -- the firing
bus, the arm pump, the sensing, the MS5607, the buzzer -- is MK1C's, from
`../mk1c/`, and `../mk1c/THEORY_OF_OPERATION.md` describes it. This file
covers what differs.

Built on the bench as a wire-wrapped test board on the second MK1C (serial
02373331FF2A, 192.168.42.1), 2026-09-29.

## What changes from MK1C

| | MK1C | MK1C-SD |
|---|---|---|
| J3, GPIO18-21 | Lua's pads | SPI0 |
| J1.6, GPIO22 | spare | the LSM6DS3's chip select |
| Lua | on core1 | off (`PYRO_HAS_LUA 0`): its pads are the bus |
| Files | littlefs | the SD card while one is mounted, littlefs otherwise (DD-076) |
| High-rate log | none | the IMU, every conversion and the flight, on the card (DD-077) |
| Bench flight | yes | yes (DD-078) |

`board.cmake` includes MK1C's and changes only these; `CMakeLists.txt` builds
MK1C's sources with this directory's headers first.

## Pins

Each J3 pad has exactly one SPI function, all on SPI0
(`rp2040-datasheet_2025-02-20.pdf`, section 2.19.2, Table 279, page 237):

| Pad | GPIO | Function |
|---|---|---|
| J3.3 | 18 | SCK |
| J3.4 | 19 | MOSI |
| J3.5 | 20 | MISO, pulled up in the pad |
| J3.6 | 21 | the SD card's chip select |
| J1.6 | 22 | the LSM6DS3's chip select |

Both selects idle high. `spi_bus_init()` sets the pins, and writes the
LSM6DS3's I2C_disable bit first thing: deselected, the part listens as an I2C
slave on SCK and MOSI, and card traffic could address it
(`LSM6DS3_DocID026899_Rev4_2015-04.pdf`, pages 32 and 55).

## The bus

One peripheral, two devices (`src/sd/spi_bus.h`). A user takes the bus with
`spi_bus_take()`, sets its own clock and mode, drives its own select, and
gives the bus back after each transaction. The card holds its busy line for
up to 250 ms after a write, 500 ms on SDXC, and the driver gives the bus back
between polls of it, so the IMU is read while the card programs. The flight
task never touches the bus: the high-rate log's reader and writer are tasks
on core1.

The clocks: the card at 12.5 MHz (`BOARD_SD_SPI_HZ`), well below its 25 MHz
default speed for the wire-wrap; the LSM6DS3 at 10 MHz, its maximum
(datasheet, page 23).

## The SD card

SPI mode, CRC on for commands and data (CMD59), which the SD specification
recommends before ACMD41 (Physical Layer Simplified Specification 6.00, PDF
page 230): on a wire-wrapped bus a corrupted byte fails as an error and a
retry, never as a wrong sector. Every wait is bounded by the specification's
limits: reads 100 ms, write busy 250 ms, 500 ms on SDXC (PDF page 97), and
the card's initialisation by 1 s. FatFs R0.16 runs over it, with exFAT, long
names and `f_expand` (`lib/fatfs`, `src/sd/ffconf.h`).

`sd_start()` runs at boot, after littlefs is mounted. A card that answers is
mounted and `vfs_mirror()` brings the configuration into line (DD-076); one
that does not leaves the board on littlefs. `POST /api/sd/init` tries again
without a reboot, and `GET /api/sd` reports the card, the FAT and the
driver's counters, with the last initialisation's responses.

**On the bench, 2026-09-29, the card never initialised: it resets.** It
answers CMD0, CMD8 (echoing 0x1AA) and CMD59, and ACMD41 answers "idle" as it
should, about 37 times; then, 28-29 ms after ACMD41 began, it stops
answering. CMD58, which a card still in SPI mode answers, gets nothing; CMD0
brings it back to idle at once. A card in SPI mode returns to SD mode only
through a power cycle (SD simplified 6.00, section 7.2.1, PDF page 228), so
it has been through a power-on reset. Brought up again, it resets again
28-29 ms later, every time: 68 times in 2 s. CRC on or off, and the interval
between polls, change nothing, and neither did 22 µF at the card.

The card sits on a carrier with no regulator, powered from J3.1, MK1C's own
3.3 V rail: U6, an XC6206 fed from VIN. VIN has no path from USB: it is
J1.3, or the TP4057 charger's battery output through JP1 (open as built).
While the card resets, the MS5607 on the same rail reads about 10 Pa low and
its step noise nearly doubles (5.8-8.4 Pa against 3.2-4.2, the largest step
32-34 Pa against 15-17, three trials each way): the card's draw as it starts
its initialisation moves the rail itself. A card draws up to 100 mA in SPI
mode (PDF page 36); U6's dropout at 100 mA is up to 680 mV (XC6206, page 5).

`POST /api/sd/init?timeout=10000&restarts=1000` keeps bringing the card up
for 10 s, so the sag can be measured at the card's VDD; `GET /api/sd`
reports each attempt (`after_r58`, `after_r0`, `restarts`, `fail_ms`).

## The LSM6DS3

±16 g and ±2000 dps (0.488 mg and 70 mdps per LSB, datasheet page 19), both
at one rate into the FIFO, 1.66 kHz by default (`POST /api/hr/start?odr=`
takes 104 to 1660). The FIFO runs in continuous mode and holds 682 sets,
410 ms at 1.66 kHz. No interrupt pin reaches the MCU, so the high-rate log's
reader polls it every 10 ms; `lsm6ds3_read()` takes whole sets only, aligned
by FIFO_PATTERN (AN4650, page 88), and a burst read ends on a set's boundary.

Measured on the bench: 1.63 kHz of sets, no FIFO overrun, 1.0 g on the axis
the board lies on.

## The high-rate log

`src/sd/hr_log.h` has the format and the tasks (DD-077). It logs while the
flight log does, launch to landing, and on the bench from
`POST /api/hr/start`; `GET /api/hr` follows it. It needs the card.
`support/hr_log.py` decodes a log into CSV.

## Known limits

- The card has not come up on this board (above), so SD-01, SD-02 and the
  logger over a flight (task SD-1) are not yet verified on hardware.
- No Lua: J3 was Lua's.
- The bus is wire-wrapped; the clocks are set for it, not for a PCB.
