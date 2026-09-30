# Datasheets

The primary sources for every figure the firmware and its documents take
from a part. Cite the file and page, not memory.

| File | Part | Revision | Source |
|---|---|---|---|
| `MS5607-02BA03_2017-06.pdf` | TE MS5607-02BA03 barometric pressure sensor (MK1B, MK1C) | 06/2017 | farnell.com/datasheets/2917207.pdf, fetched 2026-09-26; TE's later revisions would not download |
| `BST-BMP280-DS001-26_2021-10.pdf` | Bosch BMP280 barometric pressure sensor (MK1A, MK1B) | 1.26, 10/2021 | bosch-sensortec.com, bst-bmp280-ds001.pdf, fetched 2026-09-26 |
| `UM10204_I2C-bus_Rev7.0_2021-10.pdf` | NXP UM10204, the I2C-bus specification and user manual | Rev. 7.0, 1 October 2021 | pololu.com/file/0J435/UM10204.pdf, fetched 2026-09-26; NXP's link would not download |
| `TPS2595_SLVSE57C_2018-04.pdf` | TI TPS2595x eFuse, including the TPS259570 (MK1C's U9) | SLVSE57C, revised April 2018 | ti.com/lit/ds/symlink/tps2595.pdf, fetched 2026-09-26 |
| `rp2040-datasheet_2025-02-20.pdf` | Raspberry Pi RP2040 microcontroller | build 3184e62, 2025-02-20 | datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf, fetched 2026-09-26 |
| `rp2350-datasheet_2025-07-29.pdf` | Raspberry Pi RP2350 microcontroller (a candidate for a future board) | build d126e9e, 2025-07-29 | datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf, fetched 2026-09-28 |
| `W25Q128JV_RevH_2021-03-10.pdf` | Winbond W25Q128JV 128 Mbit 3 V serial flash (MK1A) | Revision H, 10 March 2021 | winbond.com/resource-files/W25Q128JV RevH 03102021 Plus.pdf, fetched 2026-09-28 |
| `W25Q16JV_RevI_2024-12-24.pdf` | Winbond W25Q16JV 16 Mbit 3 V serial flash (MK1B's U7, by its marking) | Revision I, 24 December 2024 | winbond.com/resource-files/W25Q16JV SPI RevI 12242024 Plus.pdf, fetched 2026-09-28 |
| `BY25Q64ES_Rev2.9_2024-10-29.pdf` | BYTe Semiconductor BY25Q64ES 64 Mbit 3 V serial flash (a larger part for MK1B's footprint) | Rev. 2.9, 2024-10-29 | byte-semi.com/wp-content/uploads/BY25Q64ES.pdf, fetched 2026-09-28 |
| `GD25Q64E_Rev1.4_2021-07.pdf` | GigaDevice GD25Q64E 64 Mbit 3 V serial flash (fits MK1B's footprint, not its boot stage 2) | 1.4, 2021-07-06 | uploadcdn.oneyac.com, GD25Q64ESIGR.pdf, fetched 2026-09-28 |
| `Winbond_code_storage_flash_selection_guide_2025.pdf` | Winbond code storage flash selection guide: every serial NOR part by package and voltage | 2025, dated 2025-02-18 | winbond.com, 2025-Product-Selection-Guide-Winbond-Code-Storage-Flash-Memory.pdf, fetched 2026-09-28 |
| `AP2182_AP2192_DS31569_Rev10-2.pdf` | Diodes AP2182/AP2192 dual high-side switch, no output discharge (the part MK1B's U5 should be) | DS31569 Rev. 10-2, May 2016 | diodes.com/assets/Datasheets/AP2182_92.pdf, fetched 2026-09-28 |
| `AP2182A_AP2192A_DS32193_Rev5-2.pdf` | Diodes AP2182A/AP2192A, with output discharge (MK1B's U5 as built) | DS32193 Rev. 5-2, 2022 | diodes.com/assets/Datasheets/AP2182A_92A.pdf, fetched 2026-09-28 |
| `esp32-s3_datasheet_v2.2.pdf` | Espressif ESP32-S3 series (a candidate for a future board) | v2.2 | espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf, fetched 2026-09-28 |
| `LSM6DS3_DocID026899_Rev4_2015-04.pdf` | ST LSM6DS3 accelerometer and gyroscope (MK1C J3 test board) | DocID026899 Rev 4, April 2015 | cdn.sparkfun.com/assets/learn_tutorials/4/1/6/DM00133076.pdf, fetched 2026-09-28; st.com would not download |
| `AN4650_LSM6DS3_DocID027415_Rev1.pdf` | ST AN4650, the LSM6DS3 application note: the FIFO, its pattern and burst reads | DocID027415 Rev 1 | cdn.sparkfun.com/assets/learn_tutorials/4/1/6/AN4650_DM00157511.pdf, fetched 2026-09-29; st.com would not download |
| `XC6206_ETR0305_004b.pdf` | Torex XC6206 LDO; the XC6206P332MR is MK1C's U6, the 3.3 V rail | ETR0305_004b | wmsc.lcsc.com (LCSC C5446), fetched 2026-09-28; torexsemi.com refused |
| `MIC2920A_Micrel_2005-02.pdf` | Micrel MIC2920A 400 mA LDO; the MIC2920A-3.3 feeds the MK1C J3 test board's SD card (C-U6) | Micrel, February 2005 | ww1.microchip.com/downloads/en/DeviceDoc/mic2920.pdf, fetched 2026-09-29 |
| `SD_Physical_Layer_Simplified_v6.00_2017-04.pdf` | SD Association Physical Layer Simplified Specification | Version 6.00, April 10, 2017 | academy.cba.mit.edu/classes/networking_communications/SD/SD.pdf, fetched 2026-09-28; sdcard.org would not download |

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

W25Q16JV, PDF page 70 (section 11.5): the USON 4x3 mm package, code UU:
4.00 x 3.00 mm, 0.80 mm pitch, leads 0.30 mm wide and 0.60 mm long, a
0.80 x 0.20 mm centre pad connected to nothing.

GD25Q64E, PDF page 56 (section 10.4): the USON8 3x4 mm package, code N, has
the same outline, pitch, leads and centre pad (floating). PDF page 50: the
GD25Q64ENIG. PDF pages 39-41: VCC 2.7-3.6 V. Section 7.4: 01h writes status
register 1 only, and is not executed unless CS# rises after the eighth data
bit; status register 2, with QE, takes 31h.

BY25Q64ES, page 79 (section 9.3): the USON8 4x3 mm package, the UU outline
again (0.50 mm thick against 0.55). Page 15: QE is S9, bit 1 of status
register 2. Page 29 (7.1.5): 01h takes one or two bytes, the second into
status register 2. Page 36 (7.2.6): EBh stays in continuous mode on
M5-4 = (1,0), after the mode byte and four dummy clocks. Page 45 (7.3.5):
4Bh and four dummy bytes read a 128-bit unique ID. Section 8.7: 120 MHz at
3.0-3.6 V.

Winbond selection guide, PDF pages 30-31: the only 2.7-3.6 V parts in USON-8
4x3 mm are the W25Q16JV and the W25Q32JV (UU); the W25Q32JV is no longer
made (2026-09-28). Page 39 onward: the 64 Mbit
and larger UU parts, W25Q64JW, W25Q64PW and W25Q12PW, are 1.65-1.95 V.

AP2192 (DS31569), page 1: features list reverse-current blocking and no
output discharge; 115 mOhm. Page 4: disabled, IN-to-OUT leakage at most
1 uA, reverse leakage 1 uA typical (VIN 0 V, VOUT 5 V); no discharge
resistance is specified. Page 14: AP2192MPG-13 is the MSOP-8EP on a
2,500 reel. Page 15: the MSOP-8EP drawing, the same as DS32193's (D1 1.80,
E3 2.95, e 0.65 mm). Pinout and active-high enable are the AP2192A's.

AP2192A (DS32193), page 1: output discharge; 85 mOhm. Page 4: R_DIS 100 Ohm
typical while disabled (note 6). DigiKey's attributes for AP2192MPG-13 on
2026-09-28 list "load discharge" and 85 mOhm, the AP2192A's figures; the
datasheet says otherwise.

W25Q16JV, page 64, and W25Q128JV, page 66 (AC characteristics): a page
program takes 0.4 ms typically and 3 ms at most; a 4 KB sector erase 45 ms
and 400 ms; a 64 KB block erase 150 ms and 2,000 ms. W25Q16JV page 34: a
partial page can be programmed into erased bytes "without having any
effect on other bytes within the same page". Pages 15 and 40: the SUS bit
is S15, and suspend and resume are 75h and 7Ah.

RP2040, page 123: the XIP cache is 16 kB, two-way set-associative; page
124: flushing it takes just over 1024 clock cycles. Page 418: the UART's
FIFOs are 32 deep.

RP2040, page 302 (PADS_BANK0 GPIO registers): every GPIO pad resets with
its pull-down enabled (PDE 0x1, PUE 0x0). Page 616: the pulls are 50-80 kohm.

LSM6DS3, page 23 (Table 6): SPI clock 10 MHz at most. Page 32 (Table 9):
CS high is "SPI idle mode / I2C communication enabled", and the I2C block
stays live on SCL/SPC and SDA/SDI until I2C_disable = 1 in CTRL4_C (13h),
bit 2 (page 55). Page 34: SPC is stopped high while CS is high, data driven
on the falling edge and captured on the rising (SPI mode 3). Page 51:
WHO_AM_I (0Fh) reads 0x69. Page 21: Vdd 1.71-3.6 V.

XC6206, page 3: SOT-23 dissipation 250 mW, input 7.0 V absolute maximum.
Page 4: input 6.0 V at most in operation. Page 5, 3.3 V row: 200 mA output
at least; dropout 75/350 mV (typ/max) at 30 mA and 250/680 mV at 100 mA.

XC6206, page 1: foldback current limiting, which serves as short-circuit
protection. Page 5, 3.3 V row: 200 mA maximum output, short-circuit current
100 mA typical.

MIC2920A, page 1: SOT-223 pins 1 input, 2 ground and tab, 3 output. Page 3:
400 mA guaranteed; dropout 250 mV typical at 100 mA, 370 mV at 250 mA,
400/600 mV (typ/max) at 400 mA; ground current 1.3 mA at 100 mA, 5 mA at
250 mA. Page 8: 10 µF or more on the output, ESR about 5 Ω or less,
tantalum or aluminium electrolytic; 0.1 µF on the input when a battery feeds
it; in regulation down to 1 mA of load.

SD simplified 6.00, PDF pages (the printed number is 18 less): page 36, in
SPI mode a card draws up to 0.36 W, 100 mA at 3.6 V. Pages 221-222: after 1 ms
of stable VDD the host gives at least 74 clocks with CS held high before
the first command, and in SPI mode CMD0 is that first command. Page 86: a
card's maximum current is averaged over one second. Page 228 (7.2.1): a card
enters SPI mode on CMD0 with CS low, and only a power cycle returns it to SD
mode.

LSM6DS3 registers (datasheet): CTRL1_XL (10h) ODR_XL[7:4], FS_XL[3:2]
(00 2 g, 01 16 g, 10 4 g, 11 8 g), BW_XL[1:0], page 52; CTRL2_G (11h)
ODR_G[7:4] to 1.66 kHz, FS_G[3:2] (00 245, 01 500, 10 1000, 11 2000 dps),
page 53; CTRL3_C (12h) BDU bit 6, IF_INC bit 2 (default 1), page 54;
FIFO_CTRL3 (08h) DEC_FIFO_GYRO[5:3], DEC_FIFO_XL[2:0] (001 no decimation),
pages 46-47; FIFO_CTRL5 (0Ah) ODR_FIFO[6:3] (1000 1.66 kHz), FIFO_MODE[2:0]
(110 continuous), pages 48-49; FIFO_STATUS1-4 (3Ah-3Dh): DIFF_FIFO[11:0]
unread 16-bit words, FIFO_OVER_RUN bit 6 and FIFO_EMPTY bit 4 of 3Bh,
FIFO_PATTERN[9:0], pages 69-70; the FIFO holds 8 kbyte (page 15).

AN4650, page 89 (section 8.4): "The rounding function ... is automatically
enabled when applying a multiple read operation to the FIFO output
registers", so a burst read from 3Eh returns successive FIFO words. Section
8.5.1: gyroscope and accelerometer at one ODR repeat Gx Gy Gz XLx XLy XLz.
Page 88: over SPI the FIFO status should be read in step with a data-ready
or watermark interrupt; with no interrupt pin wired, a read takes only whole
patterns, aligned by FIFO_PATTERN.
