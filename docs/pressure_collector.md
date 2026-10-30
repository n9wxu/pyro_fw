# The pressure collector: one source, no judgement

The collector is `src/pressure_collector.c` (DD-093): one interrupt state
machine that owns the sensor and its bus, runs free, and hands on every
reading with its own time. This note gives the datasheets' grounds for it,
the range of the compensation arithmetic, the bus failures it answers, and
measurements of the sensors on the bench boards. The requirements are
SNS-PRES-06, SNS-PRES-14 to SNS-PRES-17 and SNS-COL-01 to SNS-COL-06.

## The question

At the user's direction, the collector meets four needs:

1. A zero code must be impossible by design, not caught afterwards.
2. One collector, driven by interrupts, running from RAM, stamping each
   reading and handing it to the flight task.
3. No plausibility check on a compensated value, unless the compensation
   arithmetic itself can go wild outside the part's range.
4. A bus failure handled for the causes the parts' documents give.

## Summary

- **The compensation arithmetic cannot go wild on a real code.** The MS5607's
  is a straight line in the pressure code at every temperature. The BMP280's
  is smooth and monotone over the whole 20-bit code. Neither wraps or
  overflows at any coefficient. No range check is applied (SNS-PRES-06).
- **A zero code is not a measurement, and it does compensate to a wild
  value:** -152 to -202 kPa on the MS5607. The collector makes it impossible
  by timing: a read follows its own command by the part's worst-case
  conversion time, and is never repeated (SNS-COL-02).
- **One check stays, and it is arithmetic, not judgement:** the estimators
  take the logarithm of the pressure, and a value of zero or less has none.
  It is not fed to them.
- **Neither sensor stretches the clock.** The BMP280's datasheet says so. The
  MS5607's clock pin is an input.
- **The documented bus failures are answered in the collector:** each failed
  transfer is counted by cause, and three in a row clear the bus and reset
  the part (SNS-COL-04, SNS-COL-05).
- **The MS5607 is compensated to the second order below 20 C** (SNS-PRES-15),
  and a part whose PROM fails its CRC is not taken for a sensor
  (SNS-PRES-16).
- **A reading a flash operation ran beside is used like any other,** counted
  and traced (SNS-PRES-14, DD-106).

## 1. What the datasheets say

### MS5607 (`docs/datasheets/MS5607-02BA03_2017-06.pdf`)

| Page | Statement | Consequence |
|---|---|---|
| 11 | "If the conversion is not executed before the ADC read command, or the ADC read command is repeated, it will give 0 as the output result." | Zero means "no conversion to read", never a pressure. |
| 11 | "If the ADC read command is sent during conversion the result will be 0, the conversion will not stop and the final result will be wrong." | A read must never be early. An early read spoils the next result too, and that one is not zero. |
| 11 | "Conversion sequence sent during the already started conversion process will yield incorrect result as well." | A command must never overlap a conversion. |
| 3 | Conversion time at OSR 4096: 9.04 ms at most. | The read is safe 9.04 ms after the command. |
| 12 | "In the event that there is not a successful power on reset this may be caused by the SDA being blocked by the module in the acknowledge state. The only way to get the MS5607-02BA to function is to send several SCLKs followed by a reset sequence or to repeat power on reset." | The part's one documented bus fault. The cure is clocks, then the reset command. |
| 13 | "When command is sent to the system it stays busy until conversion is done." | Nothing is said about how a busy part answers its address. |
| 13 | A 4-bit CRC covers the PROM. | A corrupt coefficient is detectable at start-up. |
| 15 | Pin 8, SCLK: "I". | An input cannot hold the clock low: no clock stretching. |
| 8 | Results are specified for 10 to 1200 mbar and -40 to 85 C. | Outside that the part is less accurate, not silent. |

### BMP280 (`docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf`)

| Page | Statement | Consequence |
|---|---|---|
| 27 | "As the devices does not perform clock stretching, the SCL structure is a high-Z input without drain capability." | It cannot hold the clock low. |
| 20 | Data must be read in one burst from 0xF7 to 0xFC, or bytes of two measurements can mix. | The code does this. |
| 25 | Status bit `measuring` is 1 while a conversion runs. | A read during a conversion returns the previous data, not zero. The status bit is the only sign. |
| 12, 13 | A skipped measurement outputs 0x80000. | Not reachable in the mode used. |
| 7 | Full accuracy from 300 to 1100 hPa. | Below 300 hPa the part still answers. |

No bus fault is documented for the BMP280.

### RP2040 I2C controller (`docs/datasheets/rp2040-datasheet_2025-02-20.pdf`, section 4.3)

| Page | Statement | Consequence |
|---|---|---|
| 493, 496 | A transfer ends in an abort when the address is not acknowledged, a data byte is not acknowledged, or arbitration is lost. The controller sends the STOP itself. | These are the failures software sees. On a one-controller bus, lost arbitration means SDA was low when the controller drove it high: noise, or a target holding the line. |
| 449 | "The component does not generate a STOP if the Tx FIFO becomes empty; in this situation the component holds the SCL line low, stalling the bus until a new entry is available." | The controller stretches its own clock if software is late with the next byte. |
| 4.3.10 | A controller that is disabled while a command without STOP is in progress "continues to remain active, holding the SCL line low". The way out is IC_ENABLE.ABORT. | A transfer given up on a timeout must be aborted, not left. |
| 459 | SDA stuck low: up to nine clocks, then a STOP. | The same cure as the I2C specification, UM10204 section 3.1.16, page 19. The datasheet's register list has no control for it, so it is done by GPIO. |
| 460 | SCL stuck low: "there is no effective method to overcome this problem but to reset the bus using the hardware reset signal." | Neither sensor can cause it. A short on the board can. |
| 627 | Appendix B, errata. | No erratum is listed against the I2C block. |

## 2. The compensation arithmetic over every code

`support/compensation_range.py` runs the firmware's integer arithmetic over
the whole code range, with the datasheets' example coefficients. Both
datasheet examples reproduce.

**MS5607** (first order; below 20 C the second order moves each line by the
amounts in section 5 and leaves it straight):

| Sensor temperature | Code 0 gives | Code 2^24-1 gives | Per count | Monotone | Worst bend |
|---|---|---|---|---|---|
| -40 C | -152 200 Pa | 490 705 Pa | 38 mPa | yes | 1 Pa (rounding) |
| 20 C | -175 924 Pa | 566 027 Pa | 44 mPa | yes | 1 Pa |
| 85 C | -201 626 Pa | 647 627 Pa | 51 mPa | yes | 1 Pa |

- It is a straight line in the pressure code at any one temperature. There is
  no inflection to find.
- The sensitivity stays positive for every temperature code. It would change
  sign only below a temperature code of 0, which is -251 C.
- With every PROM coefficient at either extreme and every code at either
  extreme, the largest intermediate is 2^58. The 64-bit sums hold 2^63, and
  the 32-bit result never wraps.
- A temperature code outside -40 to 85 C moves the pressure smoothly: a
  temperature code of 0 turns 1013 hPa into 413 hPa. That is the zero-code
  case again, on the other conversion.

**BMP280:**

- Monotone over all 2^20 codes at every temperature tried, -44 to 81 C.
- The slope changes by 6 % from one end of the code range to the other. That
  is the quadratic term working as designed. There is no inflection.
- 300 hPa is near code 820 000. The line carries on below it to 100 Pa near
  code 1 008 000 and reaches zero just above that.
- The one division is by a term that is zero only if coefficient `dig_P1` is
  zero, which is a blank calibration, a start-up fault.

**Conclusion.** For any code a conversion can produce, the result is a
sensible continuation of the calibrated line, so no range check is applied.
A zero code and a failed transfer are sections 3 and 4.

**What remains** is the altitude's domain. `pp_feed_us()` in
`src/pressure_processing.c` turns each reading into an altitude. A value of
zero or less has no altitude, and one such value would put a not-a-number
into the filter for good, so `pp_feed_us()` feeds no such value to the
estimator.

## 3. Making a zero code impossible

A zero has three causes (page 11): a read with no conversion before it, a
read repeated, a read during a conversion.

The collector excludes all three by construction. An alarm handler in RAM
owns the sensor: it commands a conversion, sets an alarm for the part's
worst-case conversion time, reads, and commands the next, with no start from
any task. The only path to a read is the alarm its own command set; no read
is repeated. On the MS5607 each read follows its command by 9.1 ms against
the part's 9.04 ms, a pressure and then a temperature in each cycle. A flash
operation that holds the alarm off makes a read later, never earlier.

- A cycle is the part's raw bytes and the time of the middle of each
  conversion (SNS-PRES-08). A queue holds four cycles not yet taken; a fifth
  pushes the oldest out, counted as `pres_dropped` on `/api/status`
  (SNS-COL-03).
- The sensor task takes every cycle waiting, oldest first, each period
  (`pres_tick` in `src/hal_common/hal_common.c`), and does the arithmetic
  outside the interrupt. A period held up by up to four cycles loses
  nothing.
- The flight steps once for every sample waiting (FLT-RATE-06). The
  collector runs at its own rate, not the period's: at the 10 ms period
  (`src/loop_period.h`) a period steps none or one sample.
- A transfer is queued whole (the controller's TX FIFO holds sixteen) and its
  outcome is read at the next alarm, so the handler never waits on the bus.
- The handler and everything it calls run from RAM (SNS-COL-06), checked by
  `support/prove_core0.py`.

| Sensor, bus | A cycle | Cycles a second |
|---|---|---|
| MS5607, 400 kHz | 18.87 ms | 53 |
| BMP280, 400 kHz | 13.74 ms | 72.8 |
| BMP280, 100 kHz (MK1B) | 14.5 ms | 69 |

### The BMP280

It has the same machine with one step: a forced conversion commanded by
writing its control register, 0xF4, an alarm at its 13.3 ms worst case (Table 13, page 18),
and one burst read of six bytes from 0xF7 (`BMP280_PART` in
`src/bmp280_driver.h`). Its read during a conversion returns old data rather
than zero, so the timing is the only guard, as on the MS5607.

### What a flash operation does to the readings

Two separate effects:

- **Timing.** On the ground a flash operation holds interrupts off on both
  cores, and the handler runs late. The conversion's result is latched in
  the part, so the reading is intact and its stamp is the command's. Only
  the next command is late. An erase leaves a gap in the readings of up to
  about 60 ms (DD-106). From launch to landing an operation runs on core1
  alone (DD-104), and the collector's handler, on core0, is not held.
- **Electrical.** A flash operation beside a conversion disturbs the
  conversion itself: much on MK1B's MS5607, slightly on MK1C's, not
  measurably on the BMP280 (the measurements below). No collector design
  removes this. The reading is used like any other; the collector marks it,
  and it is counted (`pres_flashed` on `/api/status`) and traced ('F' a
  pressure, 'G' a temperature) (SNS-PRES-14, DD-106). The flight's filters
  are trusted to absorb it.

## 4. Bus failures robust code must answer

| Cause | Documented where | Seen by the controller as | Answer |
|---|---|---|---|
| Sensor in reset or unpowered | MS5607 p. 10-11 (2.8 ms PROM reload) | address not acknowledged | count it, try again 1 ms on |
| SDA held low by the sensor | MS5607 p. 12; UM10204 3.1.16 | arbitration lost, or address not acknowledged | nine clocks and a STOP, then the part's reset command and its start-up |
| Processor reset in the middle of a read, sensor still powered | UM10204 3.1.16 | the same, at start-up | the same; start-up also clears the bus before detection |
| Software late with the next byte | RP2040 p. 449 | the controller holds SCL low | the collector queues the whole transfer at once |
| Transfer given up on a timeout | RP2040 4.3.10 | controller left mid-command, SCL low | IC_ENABLE.ABORT before anything else |
| SCL shorted low | RP2040 p. 460 | nothing completes | none; the sensor is reported lost (SNS-PRES-11, SNS-PRES-17) |

Not causes: clock stretching by either sensor, and RP2040 I2C errata. There
are none.

**What the code does.**

- At start-up: resets the I2C block, clocks the bus nine times and sends a
  STOP by GPIO (`src/i2c_recover.c`), then detects the part with the SDK's
  calls, each bounded by a timeout. MS5607 detection sends the reset and
  waits 3 ms before reading the PROM, as page 12 asks.
- In the collector: a failed transfer is counted by cause, and the cycle
  starts again 1 ms on; a failed cycle is not handed on. A transfer that did
  not finish is aborted (RP2040 4.3.10). The causes are on `/api/status` as
  `pres_bus` (`address_nack`, `data_nack`, `line_held`, `timeout`), their sum
  as `pres_rejects`.
- Three failures in a row: the pads are taken from the controller, the
  controller is reset, nine clocks and a STOP go out at 10 us an edge, the
  controller is set up again as it was, the part's reset command is sent,
  and its start-up is waited out (MS5607 3 ms, BMP280 2 ms). About 4 ms.
  Each recovery is counted as `pres_recoveries`.

## 5. Compensation and the PROM check

- **Second-order compensation.** The datasheet (page 9) recommends it
  "particularly in low temperature", and `ms5607_compensate_prom()` applies
  it below 20 C (SNS-PRES-15). With the example coefficients it lowers the
  first order's reading by:

  | Sensor temperature | At 1013 hPa | At 10 hPa |
  |---|---|---|
  | 20 C and above | 0 | 0 |
  | 0 C | 277 Pa | 1 Pa |
  | -15 C | 878 Pa | 1 Pa |
  | -40 C | 4696 Pa | 48 Pa |

  Heights above the pad depend little on it while the sensor's temperature
  holds, the pad reference and the flight sharing the term. It matters for
  the absolute pressure, and for a flight where the bay cools.
- **The PROM's CRC.** Detection takes a part only when its PROM's first word
  is neither 0 nor 0xFFFF and its 4-bit CRC (page 13; TE's AN520) checks
  (`ms5607_prom_crc_ok()`, SNS-PRES-16). One corrupt coefficient would give
  a wrong but smooth pressure for the whole flight.

## 6. The collector's design

| # | Decision | Taken |
|---|---|---|
| 1 | Collector | free-running, every reading kept; a queue of four, the oldest pushed out and counted |
| 2 | The range check and the zero check | none; only "p > 0", where the logarithm is taken |
| 3 | A reading with a flash operation beside it | used like any other, counted and traced (SNS-PRES-14, DD-106) |
| 4 | In-flight bus recovery | states of the collector's machine |
| 5 | Second-order compensation | in the sensor task |
| 6 | The PROM's CRC at start-up | checked; a failure is no sensor |

On the bench, 2026-10-03:

| Board | Sensor | Readings a second | Interval, min / median / max | Zeros | Bus failures |
|---|---|---|---|---|---|
| MK1C-SD | MS5607 | 53.0 | 18.87 / 18.87 / 18.96 ms | 0 | 0 |
| MK1C | MS5607 | 53.1 | 18.87 / 18.87 / 18.92 ms | 0 | 0 |
| MK1B | MS5607 | 53.1 | 18.87 / 18.87 / 18.95 ms | 0 | 0 |
| MK1A | BMP280 | 72.8 | 13.74 / 13.74 / 13.81 ms | 0 | 0 |

All three MS5607 parts pass their CRC. The recovery states bring a part back
on the host's fake bus (`test/test_collector.c`); on the bench they have run
only against the BMP280 below, which did not answer until a power cycle.

### A BMP280-fitted MK1B, measured 2026-10-03

A second MK1B, with the BMP280, measured with
`support/pressure_disturbance.py` beside the bench MK1B with the MS5607.
Departures are from a 9-point running median, so slow drift does not count.

| Board | At rest, rms | During beep codes, rms | Worst at rest / beeping | Raw temperature code, rest / beeping |
|---|---|---|---|---|
| MK1B, BMP280 | 2.5 Pa | 3.1 Pa | 8 / 14 Pa | 32 / 46 |
| MK1B, MS5607 | 9.8 Pa | 11.4 Pa | 33 / 45 Pa | 239 / 383 |

- **The beep disturbs both a little, and the same way:** the rms rises by
  about a fifth on each, and the temperature code's by about half.
- **On the MS5607 board, conversions beside a flash operation** sat 15.3 Pa
  rms from their neighbours against 10 Pa for the rest: 41 of them in 68
  programs and 9 erases.
- **On the BMP280 board flash operations disturb nothing.** Four runs after
  a power cycle, 59 programs and 8 or 9 erases each: the 62 to 64 conversions
  marked as flashed sat 2.3 to 3.0 Pa rms from their neighbours, which is
  the board's ordinary scatter.
- **On USB alone the BMP280 left the bus once.** In the first run, 20.4 s
  into the beep phase, during the fourth beep code, it stopped acknowledging
  its address. The readings up to that one were ordinary. The collector
  counted the failures by cause and ran its recovery (bus clear, soft reset,
  2 ms) over 2500 times in a minute without the part answering. After a restart without
  removing power, start-up's own bus clear and reset at both addresses did
  not find it either: the board went to FAULT, `sensor_fail`. A power cycle
  brought it back. In four further runs, 16 beep codes, it did not happen
  again.
- **The cause is the bench's power, not the board in flight.** The buzzer is
  driven from VUSB, and with no battery fitted the USB supply's limited
  current lets a beep pull the 3.3 V rail down (the designer, 2026-10-03).
  With a battery fitted to the BMP280 board, four runs:

  | BMP280 MK1B | At rest, rms | Beeping, rms | Worst beeping | Temperature code, rest / beeping |
  |---|---|---|---|---|
  | USB alone | 2.4 to 2.5 Pa | 3.1 to 3.4 Pa | 13 to 19 Pa | 32 / 46 to 51 |
  | Battery fitted | 1.9 to 2.3 Pa | 1.9 to 2.4 Pa | 6 to 10 Pa | 16.5 / 17 to 19 |

  On the battery a beep changes nothing, the sensor is quieter even at rest,
  and the temperature code's scatter is half what it is on USB alone. No
  transfer failed in 16 beep codes. The rail itself was not measured.

  The battery was then moved to the MS5607 board, four runs:

  | MS5607 MK1B | At rest, rms | Beeping, rms | Worst at rest | Beside a flash operation, rms |
  |---|---|---|---|---|
  | USB alone | 9.8 Pa | 11.4 Pa | 33 Pa | 15.3 Pa |
  | Battery fitted | 2.3 to 2.4 Pa | 2.4 Pa | 7 to 9 Pa | 8.5 to 17.0 Pa |

  On the battery this sensor is at its datasheet's 2.4 Pa (page 4), a
  quarter of what it showed on USB alone, and a beep changes nothing.
- **Flash operations disturb the MS5607 on MK1B, battery or not.** The
  32 to 37 conversions marked as flashed in each run sat 8.5 to 17 Pa rms
  from their neighbours, 43 Pa at worst, against 2.3 Pa for the rest. That
  is the board, not the bench. They are used like any other (SNS-PRES-14,
  DD-106).
- **MK1C on a battery, four runs:** 2.4 to 2.6 Pa rms at rest, against the
  6 to 7 Pa it showed on USB alone, and the same while beeping. Conversions
  beside a flash operation sat 3.4 to 3.9 Pa rms from their neighbours, 12 Pa
  at worst: a slight disturbance, a quarter of MK1B's.

  | On a battery | At rest, rms | Beeping, rms | Beside a flash operation, rms | Worst beside one |
  |---|---|---|---|---|
  | MK1B, MS5607 | 2.3 to 2.4 Pa | 2.4 Pa | 8.5 to 17.0 Pa | 43 Pa |
  | MK1C, MS5607 | 2.4 to 2.6 Pa | 2.5 to 2.6 Pa | 3.4 to 3.9 Pa | 12 Pa |
  | MK1B, BMP280 | 1.9 to 2.3 Pa | 1.9 to 2.4 Pa | 2.0 to 2.9 Pa | 7 Pa |

- **The 9 Pa figure is a bench artefact.** The host tests fly at 1.2 and
  9 Pa (DD-091, `test/board_harness.h`); 9 Pa is MK1B's MS5607 on USB alone.
  That sensor on a battery is 2.3 Pa.
- **What the design says:** the BMP280 datasheet, page 27, section 5.1: once
  CSB has been pulled low, "the I2C interface is disabled until the next
  power-on-reset". On MK1B's schematic CSB, SDO, VDDIO and VDD of U4 are all
  on the 3.3 V net, so only a dip of that rail reaches CSB. Whether the beep
  did that is not established.
- **A sensor silent on the pad is a general fault** (SNS-PRES-17): announced
  and named on `/api/status` until the board is restarted.
