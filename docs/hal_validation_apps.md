# HAL validation applications

Options for HAL-06 and BLD-06: each board's HAL validated on hardware,
independently of the flight software, by an application built for that
purpose. BRD-02 depends on it: every value a board declares in its
`THEORY_OF_OPERATION.md` is to be confirmed by that validation. Nothing here
is built yet.

## What has to be validated

`src/hal.h` is the seam (HAL-05). The host suites prove the flight software
against a mock of it; nothing proves a board's implementation of it except a
flight. Per board, against test equipment:

| HAL area | What to measure | Equipment |
|---|---|---|
| Pressure | sample rate, the stamp against a reference clock, noise at rest, the rated range | a chamber or a reference barometer, a logic analyser on the bus |
| Pyro health | each fault the board declares it can detect, and none it cannot | a resistor box and a switch: open, match, short |
| Pyro fire | pulse length and current into a dummy load; the gap between channels; a pulse into a short; protection released before the next pulse | a load, a current probe, a scope |
| Disarm | a stopped loop leaves nothing energised, and in what time (PYR-ARM-01) | a scope on the firing bus |
| Storage | the log's write time and its effect on the loop (FLT-RT-01); a reset mid-write | the board's own counters |
| Buzzer, ground test switch | the pattern; the switch read both ways, pad to pad and across the buzzer's pad | a scope |
| Reset cause, the resume record | each cause reported; the record surviving each | a supply that can be interrupted |

## Options

### A. A second firmware image per board

A `hal_validate_<board>` target: the board package and `src/hal_common`
linked with a small main in place of `main_hardware.c`, with no flight
software. It runs a script of HAL calls and reports over the telemetry UART.

- For: it is what HAL-06 says, the HAL with nothing above it. A fault found is
  the HAL's.
- Against: `hal_common` today pulls in the storage task, FreeRTOS and the
  network for its own services, so the image needs most of the firmware's
  link closure, or `hal_common` needs splitting first. The telemetry port
  takes no commands (TEL-12 is the flight firmware's rule, not this image's,
  but the HAL has no receive call), so the script is fixed or chosen by the
  ground test switch.
- Cost: the split of `hal_common`, a build target per board, and the script.

### B. A validation mode in the flight firmware, over HTTP

Routes under `/api/validate/` that call the HAL directly, refused unless the
board is on USB in PAD_IDLE with test mode on.

- For: no second image; a host script drives it and collects results, as
  `support/api_check.py` and `support/bench_flight.py` already do.
- Against: it is not independent of the flight software, which HAL-06 asks
  for, and it puts fire-on-command code in the flight image. CODE-09 forbids
  test-only constructs in flight code; a validation route is one.
- Cost: small. Not recommended for that reason.

### C. A, built on the host-side tools that exist

Option A for what must run on the board with nothing above it (pyro fire,
disarm, the pressure stamp), and the existing bench scripts for what is
already measured through the running firmware's own reports
(`loop_late_max_us`, `sample_interval_us`, `noise_mpa`, the pressure trace,
the bench flight). The validation record per board is one file,
`boards/<name>/VALIDATION.md`, with each declared value, how it was measured
and the result.

- For: the smallest image that satisfies HAL-06; nothing already measurable
  is measured twice.
- Against: two kinds of evidence in one record.
- Cost: as A, less the storage and network parts of the script.

## What each option needs first

- A and C: `hal_common` split so the pyro, pressure, buzzer and switch parts
  link without the storage task and the network. This is the real work.
- All: a decision on how the image is told what to run. The ground test
  switch is the only input the HAL has.
- All: the equipment list above agreed per board, since it sets what
  "confirmed" means for BRD-02.

## Open questions for the bench

1. MK1A and MK1B end a pulse from the loop, so a stopped loop leaves a channel
   energised until the watchdog, 1 s. PYR-ARM-01 asks for 50 ms. Is the bound
   to be met on those boards (a hardware timer that ends the pulse, or a
   shorter watchdog while a pulse runs), or is it MK1C's alone?
2. MK1B's PTC reset time under a shorted match sets its `fire_gap`. It has not
   been measured.
3. The pulsed read of the ground test switch on the buzzer's pad is
   unverified on hardware (`docs/ground_test_on_buzzer_pad.md`, its bench
   list).
