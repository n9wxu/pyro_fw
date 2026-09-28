# Datasheets

The primary sources for every figure the firmware and its documents take
from a part. Cite the file and page, not memory.

| File | Part | Revision | Source |
|---|---|---|---|
| `MS5607-02BA03_2017-06.pdf` | TE MS5607-02BA03 barometric pressure sensor (MK1B, MK1C) | 06/2017 | farnell.com/datasheets/2917207.pdf, fetched 2026-09-26; TE's later revisions would not download |
| `BST-BMP280-DS001-26_2021-10.pdf` | Bosch BMP280 barometric pressure sensor (MK1A) | 1.26, 10/2021 | bosch-sensortec.com, bst-bmp280-ds001.pdf, fetched 2026-09-26 |
| `UM10204_I2C-bus_Rev7.0_2021-10.pdf` | NXP UM10204, the I2C-bus specification and user manual | Rev. 7.0, 1 October 2021 | pololu.com/file/0J435/UM10204.pdf, fetched 2026-09-26; NXP's link would not download |
| `TPS2595_SLVSE57C_2018-04.pdf` | TI TPS2595x eFuse, including the TPS259570 (MK1C's U9) | SLVSE57C, revised April 2018 | ti.com/lit/ds/symlink/tps2595.pdf, fetched 2026-09-26 |
| `rp2040-datasheet_2025-02-20.pdf` | Raspberry Pi RP2040 microcontroller | build 3184e62, 2025-02-20 | datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf, fetched 2026-09-26 |
| `rp2350-datasheet_2025-07-29.pdf` | Raspberry Pi RP2350 microcontroller (a candidate for a future board) | build d126e9e, 2025-07-29 | datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf, fetched 2026-09-28 |
| `esp32-s3_datasheet_v2.2.pdf` | Espressif ESP32-S3 series (a candidate for a future board) | v2.2 | espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf, fetched 2026-09-28 |

MS5607, page 3: conversion time at OSR 4096 is 7.40 / 8.22 / 9.04 ms (min /
typ / max). Page 4 (pressure output): RMS resolution 0.024 mbar (2.4 Pa) at
OSR 4096, 0.036 at 2048, 0.054 at 1024, 0.084 at 512, 0.130 at 256.
Page 5 (digital inputs): the serial data clock in I2C is 400 kHz at most.

BMP280, page 27 (section 5, 5.2): the I2C interface follows the Philips
specification 2.1 and supports its standard, fast and high-speed modes; page
2 gives I2C up to 3.4 MHz, high-speed mode's rate. Page 31 (5.4.2) times
standard and fast modes together and high-speed mode apart. Fast-mode plus is
not among them.

RP2040, section 4.3.2 (page 441): each I2C instance supports standard, fast
or fast-mode plus, "not High speed".

UM10204, page 44 (Table 10): the rise time of SDA and SCL is at most
1000 ns in standard mode, 300 ns in fast mode and 120 ns in fast-mode plus.
Page 50 (Equation 1): Rp(max) = tr / (0.8473 x Cb); 4k7 holds fast mode's
300 ns to about 75 pF of bus.

TPS2595, page 3 (device options): the TPS259570 has no output-voltage clamp,
latches off, and has no quick output discharge. Page 5 (absolute maximum
ratings): OUT from -0.3 V to VIN + 0.3 V. Section 9.2.5: disabled, the output
is left floating (page 30). Nothing specifies current into OUT while disabled.

RP2040, section 2.19.2, Table 279: GPIO2-5 are SPI0's SCK, TX, RX and CSn.

RP2350, page 13: 520 kB of SRAM in 10 banks; up to 16 MB of QSPI flash or
PSRAM, and another 16 MB through an optional second chip select. Page 21: QMI
CS1n is on GPIO0, 8, 19 or 47. Page 347: the second window (0x11000000) is
read-only until XIP_CTRL.WRITABLE_M1 is set for a RAM device. Page 593: a
Bank 0 GPIO used as the second chip select needs an external pull-up. Page
1227: M0/M1_TIMING.MAX_SELECT bounds chip-select time, for PSRAM refresh.
Page 1235 (section 12.14.5): in direct mode the XIP window is disconnected,
and any access to it is a bus fault. Pages 387-388: the bootrom's erase and
program keep the QMI in direct mode for their whole duration. Page 1366:
erratum RP2350-E9, on the A2 stepping, a Bank 0 input between VIL and VIH
leaks about 120 uA and holds itself near 2.2 V; a pull-down cannot overcome
it, 8.2 kohm or less to ground can.

ESP32-S3, pages 3-4: two cores at up to 240 MHz, a single-precision FPU,
512 KB of SRAM, an SD/MMC host controller with two slots. Page 13 (Table
1-1): octal-PSRAM variants are rated to 65 C ambient; the in-package flash
does not support auto suspend by default. Page 20: flash and PSRAM share the
SPI0/1 interface. Page 39: the instruction and data caches are shared by
both cores. What a flash write does to the caches is in ESP-IDF, not the
datasheet: "Concurrency Constraints for Flash on SPI0/1" (ESP-IDF
Programming Guide v6.1), fetched 2026-09-28.
