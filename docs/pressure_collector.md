# The pressure collector: one source, no judgement

2026-10-03. A design note with options. **Decided and built the same day
(DD-093):** the free-running collector with every reading kept in a queue of
four, recovery as states of its machine, second-order compensation and the
PROM's CRC. The code is `src/pressure_collector.c`. Sections 1 to 5 are the
record of what was found; section 6 records the decisions.

## The question

The flight software discards a pressure reading in four cases
(`ms5607_tick()` in `src/hal_common/hal_common.c`): the transfer failed, the
raw code was zero, the compensated pressure was outside 1 Pa to 130 kPa, or a
flash operation ran beside the conversion. The first three are judgements
made after the fact. The direction asked for is:

1. A zero code must be impossible by design, not caught afterwards.
2. One collector, driven by interrupts, running from RAM, stamping each
   reading and handing the newest to the flight task.
3. No plausibility check on a compensated value, unless the compensation
   arithmetic itself can go wild outside the part's range.
4. A bus failure handled for the causes the parts' documents give.

## Summary

- **The compensation arithmetic cannot go wild on a real code.** The MS5607's
  is a straight line in the pressure code at every temperature. The BMP280's
  is smooth and monotone over the whole 20-bit code. Neither wraps or
  overflows at any coefficient. The range check can go.
- **A zero code is not a measurement, and it does compensate to a wild
  value:** -152 to -202 kPa on the MS5607. So it has to be impossible, as
  asked. The present handler already makes it so, by timing. On the bench the
  zero path has never been taken: four boards, 15 to 28 hours each, 0
  rejects.
- **One check has to stay, and it is arithmetic, not judgement:** the
  estimators take the logarithm of the pressure. A value of zero or less has
  no logarithm.
- **Neither sensor stretches the clock.** The BMP280's datasheet says so. The
  MS5607's clock pin is an input.
- **Documented bus failures exist, and the code handles them at start-up only.**
  In flight a failed transfer is retried 50 ms later with no abort and no bus
  clear.
- **Two things found on the way.** The MS5607's second-order temperature
  compensation is not implemented: below 20 C the reading is high by 277 Pa at
  0 C and 878 Pa at -15 C. The MS5607's PROM carries a CRC that the code does
  not check.

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

**MS5607** (first order, as coded):

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
sensible continuation of the calibrated line. The 1 Pa to 130 kPa check
protects against nothing the arithmetic does. It only ever caught the zero
code and a failed transfer, and those are sections 3 and 4.

**What must remain** is the logarithm's domain. `pressure_estimator.c` and
`estimator_lumped.c` take ln(p). A value of zero or less is not usable, and
one such value would put a not-a-number into the filter for good. That guard
belongs where the logarithm is taken, in `pressure_processing.c`, as "no
reading".

## 3. Making a zero code impossible

A zero has three causes (page 11): a read with no conversion before it, a
read repeated, a read during a conversion.

**Today** (`src/ms5607_oneshot.c`, DD-051, DD-066). The loop starts a pair.
An alarm handler in RAM then owns the sensor: it commands the pressure
conversion, sets an alarm 9.1 ms on, reads, commands the temperature, sets
the alarm, reads. Each read follows its own command by 9.1 ms, against the
part's 9.04 ms. No read is repeated. A flash operation that holds the alarm
off makes a read later, never earlier. So the three causes are already
excluded, and the bench agrees:

| Board | Up for | Zero or range rejects | Loop arrived early (`pres_waits`) | Flash beside a conversion |
|---|---|---|---|---|
| MK1B (MS5607) | 27.6 h | 0 | 20 | 100 |
| MK1C (MS5607) | 27.0 h | 0 | 0 | 0 |
| MK1C-SD (MS5607) | 27.5 h | 0 | 0 | 0 |
| MK1A (BMP280) | 15.4 h | 0 | 0 | 0 |

What is not by design today: the loop starts every pair. When the loop comes
round before the pair has finished it finds the collector busy and waits a
loop. That is the 20 above. The collector's rate is the loop's rate.

### Option A: leave it

The zero check stays as a check that never fires. Cheapest. It keeps a
judgement in the path, which is what was asked to go.

### Option B: a free-running collector, newest value taken

The handler restarts itself: pressure, temperature, pressure, with no start
from the loop. A pair takes 18.2 ms of conversion and about 0.4 ms of
transfers, so it produces about 53 pairs a second against the loop's 50. It
publishes each finished reading, with its stamp, to a slot the flight task
reads. The flight task takes the newest at its own cadence.

- A zero is impossible by construction: the only code path to a read is the
  alarm set by that read's own command.
- The loop cannot arrive early: there is always a reading newer than the last
  one it took. `pres_waits` goes.
- About three readings a second are never taken, because two arrived in one
  loop.
- The slot needs a sequence count so the flight task never reads half of an
  update.

### Option C: free-running, every reading kept (recommended)

As B, with a queue of two or three instead of one slot. The estimators work
from each reading's own time stamp, so an extra reading in a loop is more
data at no cost in design. Nothing is dropped, and a loop held up for 40 ms
loses nothing either.

### The BMP280

It has no interrupt handler today. The loop commands a forced conversion and
reads it a loop later with the SDK's calls, which run from flash. Under B or C
it gets the same handler shape: command, alarm at the conversion time, burst
read. Its read during a conversion returns old data rather than zero, so the
timing is the only guard, as on the MS5607.

### What flash writes still do

Two separate effects:

- **Timing.** A flash operation holds interrupts off. The handler runs late.
  The conversion's result is latched in the part, so the reading is intact
  and its stamp is the command's. Only the next command is late.
- **Electrical.** On MK1B a flash operation beside a conversion disturbs the
  conversion itself (DD-068: 9 Pa of residual became 21 to 25 Pa, readings
  up to 91 Pa off). MK1C does not show it. No collector design removes this.
  It needs the board's supply fixed, or the write placed between conversions,
  or the reading marked. Today it is discarded (SNS-PRES-14).

Under "all values from the sensor are valid", the consistent choice is to
hand the flash-marked reading on with its mark, and let the estimator's own
outlier rule deal with it. That is a decision, listed below.

## 4. Bus failures robust code must answer

| Cause | Documented where | Seen by the controller as | Answer |
|---|---|---|---|
| Sensor in reset or unpowered | MS5607 p. 10-11 (2.8 ms PROM reload) | address not acknowledged | count it, try again next cycle |
| SDA held low by the sensor | MS5607 p. 12; UM10204 3.1.16 | arbitration lost, or address not acknowledged | nine clocks and a STOP, then the MS5607 reset command and its 2.8 ms |
| Processor reset in the middle of a read, sensor still powered | UM10204 3.1.16 | the same, at start-up | the same; done today at start-up |
| Software late with the next byte | RP2040 p. 449 | the controller holds SCL low | queue the whole transfer at once; the MS5607 handler does, the SDK's calls do not |
| Transfer given up on a timeout | RP2040 4.3.10 | controller left mid-command, SCL low | IC_ENABLE.ABORT before anything else |
| SCL shorted low | RP2040 p. 460 | nothing completes | none; report the sensor lost |

Not causes: clock stretching by either sensor, and RP2040 I2C errata. There
are none.

**What the code does today.**

- At start-up: resets the I2C block, clocks the bus nine times and sends a
  STOP by GPIO (`src/i2c_recover.c`), then sends the MS5607 reset and waits
  3 ms. This matches MS5607 page 12.
- In flight: a failed pair is marked, the loop backs off 50 ms and starts
  another. There is no abort and no bus clear. A sensor holding SDA low stays
  that way until the next power-up.
- A timeout in the handler returns without an abort. So does the SDK's
  timeout path, which the BMP280 and the detection use.
- A failure is counted in `pres_rejects` with the zero and range cases. The
  cause is not kept.

**Proposed.** The collector owns recovery, as states of the same handler:

1. A failed transfer: abort, count it by cause, try the next cycle.
2. Three failures in a row: nine clocks and a STOP, the reset command, the
   PROM reload time, then carry on. About 4 ms.
3. `/api/status` carries one counter per cause, so a bench or a flight says
   which one happened.

## 5. Found on the way

- **Second-order compensation is missing.** The datasheet (page 9) recommends
  it "particularly in low temperature". `ms5607_compensate_prom()` is first
  order only. With the example coefficients the reading is high by:

  | Sensor temperature | At 1013 hPa | At 10 hPa |
  |---|---|---|
  | 20 C and above | 0 | 0 |
  | 0 C | 277 Pa | 1 Pa |
  | -15 C | 878 Pa | 1 Pa |
  | -40 C | 4696 Pa | 48 Pa |

  The pad reference and the flight share the error while the sensor's
  temperature holds, so heights above the pad are little affected. It matters
  for the absolute pressure, and for a flight where the bay cools.
- **The PROM's CRC is not checked.** Detection accepts any PROM whose first
  word is not 0 or 0xFFFF. One corrupt coefficient would give a wrong but
  smooth pressure for the whole flight.

## 6. What was decided

| # | Decision | Taken |
|---|---|---|
| 1 | Collector | free-running, every reading kept; a queue of four, the oldest pushed out and counted |
| 2 | The range check and the zero check | removed; only "p > 0", where the logarithm is taken |
| 3 | A reading with a flash operation beside it | unchanged: discarded and counted on every board (SNS-PRES-14) |
| 4 | In-flight bus recovery | built, as states of the collector's machine |
| 5 | Second-order compensation | built, in the sensor task |
| 6 | The PROM's CRC at start-up | built; a failure is no sensor |

On the bench after the change:

| Board | Sensor | Readings a second | Interval, min / median / max | Zeros | Bus failures |
|---|---|---|---|---|---|
| MK1C-SD | MS5607 | 53.0 | 18.87 / 18.87 / 18.96 ms | 0 | 0 |
| MK1C | MS5607 | 53.1 | 18.87 / 18.87 / 18.92 ms | 0 | 0 |
| MK1B | MS5607 | 53.1 | 18.87 / 18.87 / 18.95 ms | 0 | 0 |
| MK1A | BMP280 | 72.8 | 13.74 / 13.74 / 13.81 ms | 0 | 0 |

All three MS5607 parts passed their CRC. The recovery states have run on the
host's fake bus only: no bench board has had a bus fault to recover from.

A 3 km bench flight on MK1C-SD passed: launch at 32 m, apogee called at the
profile's apogee, peak 3000.06 m against 3000, main at 297.8 m against 300,
landing at 0.1 m, no cycle pushed out of the queue. The first attempts
failed, and found two things the host could not: the flight step took one
sample a loop from a collector giving 53 (FLT-RATE-06), and a landed bench
profile was a stuck sensor (SIM-02). Both are fixed (DD-093).

### A BMP280-fitted MK1B, measured 2026-10-03

A second MK1B, with the BMP280 (serial 0268FFB038CD), was loaded and measured
with `support/pressure_disturbance.py`, beside the bench MK1B with the MS5607.
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
  the board's ordinary scatter. Discarding them (SNS-PRES-14) gains nothing
  on this board.
- **The BMP280 left the bus once.** In the first run, 20.4 s into the beep
  phase, during the fourth beep code, it stopped acknowledging its address.
  The readings up to that one were ordinary. The collector counted the
  failures by cause and ran its recovery (bus clear, soft reset, 2 ms) over
  2500 times in a minute without the part answering. After a restart without
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
  and the temperature code's scatter is half what it was. No transfer failed
  in 16 beep codes. The rail itself was not measured. The MS5607 board has
  not been run on a battery.
- **What the design says:** the BMP280 datasheet, page 27, section 5.1: once
  CSB has been pulled low, "the I2C interface is disabled until the next
  power-on-reset". On MK1B's schematic CSB, SDO, VDDIO and VDD of U4 are all
  on the 3.3 V net, so only a dip of that rail reaches CSB. Whether the beep
  did that is not established.
- **What the firmware did not do:** while the sensor was silent on the pad,
  `/api/status` listed no fault, and the estimator still reported that it
  explained the readings: its last sample. No requirement asks for a lost
  sensor to be reported on the pad; SNS-PRES-11 asks it in flight.

SNS-PRES-06 is reworded, and SNS-COL-01 to SNS-COL-06, SNS-PRES-15 and
SNS-PRES-16 are new.
