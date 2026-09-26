# Datasheets

The primary sources for every figure the firmware and its documents take
from a part. Cite the file and page, not memory.

| File | Part | Revision | Source |
|---|---|---|---|
| `MS5607-02BA03_2017-06.pdf` | TE MS5607-02BA03 barometric pressure sensor (MK1B, MK1C) | 06/2017 | farnell.com/datasheets/2917207.pdf, fetched 2026-09-26; TE's later revisions would not download |
| `BST-BMP280-DS001-26_2021-10.pdf` | Bosch BMP280 barometric pressure sensor (MK1A) | 1.26, 10/2021 | bosch-sensortec.com, bst-bmp280-ds001.pdf, fetched 2026-09-26 |
| `UM10204_I2C-bus_Rev7.0_2021-10.pdf` | NXP UM10204, the I2C-bus specification and user manual | Rev. 7.0, 1 October 2021 | pololu.com/file/0J435/UM10204.pdf, fetched 2026-09-26; NXP's link would not download |
| `rp2040-datasheet_2025-02-20.pdf` | Raspberry Pi RP2040 microcontroller | build 3184e62, 2025-02-20 | datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf, fetched 2026-09-26 |

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
