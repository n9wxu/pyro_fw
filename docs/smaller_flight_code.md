# Smaller flight code: what runs from launch to landing, and a smaller version at 100 Hz

The question, as asked: "Review the flight code for algorithm/architecture
changes that will reduce code/ram size. I want a deep dive into a smaller
version that performs the same tasks at the same or faster (100hz) rate."

The study serves a second request, called request (b) below: "Move all
flight code (launch to landing) to RAM so no flash executed after launch
until after landing" (the USB flight test mode is exempt). The RAM request
(b) costs is the size of the launch-to-landing code and the read-only data
that code reads, so that size is the measure used throughout.

Every file:line below refers to commit 48da822 (branch `alfs-integration`).
Every size is in bytes unless it says kB. A measured figure in kB is
thousands of bytes; the nominal size of a memory or a buffer (a 4 kB scratch
bank, the 16 kB XIP cache, MK1B's 384 kB slot, the 2 kB heap reservation) is
in kilobytes of 1,024. Figures marked "estimate" are not measured; every
other figure says how it was measured.

---

## 1. Bottom line

- **Today the launch-to-landing path is 66.0 kB of code on MK1C** (62,872
  bytes executed from flash, 3,120 already in RAM) **and reads about 3 kB of
  read-only data**. MK1B's path is 61,814 bytes of code and 3,718 of data;
  MK1A's 61,456 and 2,899. Measured by a call graph of the linked images
  (section 2).
- **Running that path from RAM needs about 66 kB on MK1C today, and the board
  has 16.8 kB free** (12,752 bytes above `.bss` in main SRAM, plus 2 kB at the
  bottom of each scratch bank). The shortfall is about 49 kB.
- **A version that performs the same tasks is 40.5 kB of code and 2.6 kB of
  data on MK1C** (37.7 kB and 37.4 kB of code on MK1B and MK1A), measured on
  images built with the size changes of section 5.2 and on their call graphs
  with the removed calls taken out. The code that replaces what is removed
  adds about 1.5 kB (estimate). The same changes give back 10.1 kB of RAM
  (measured).
- **100 Hz holds with room to spare.** Each sample costs about 0.7 ms in
  ascent and 0.9 ms in descent today (estimate, from operation counts and the
  ROM's float timings), 9 % of core0 at 100 Hz. The smaller version needs
  about 0.3 to 0.37 ms a sample (estimate), under 5 % of core0. The sensors
  reach 100 Hz with less oversampling and about the same information each
  second (estimate): the MS5607 at OSR 2048 for both conversions (about 101
  pairs a second), the BMP280 forced at x2 (101 to 110 a second).
- **With the smaller version, flight-in-RAM fits once lwIP is replaced.**
  smallest_tcp gives back 38,480 bytes of RAM (measured on the spike,
  `docs/smallest_tcp_evaluation.md` on `origin/smallest-tcp-spike`). Free RAM
  then reaches about 66 kB on MK1C against about 42 kB needed (estimate):
  about 24 kB of margin. Without smallest_tcp, the smaller version is still
  about 15 kB short. Today's path does not fit even with smallest_tcp.

The changes that matter most, measured on MK1C as code taken off the
launch-to-landing path:

| # | Change | Code off the path | RAM back | Requirement touched |
|---|---|---|---|---|
| 1 | Build the flight path for size (`-Os`), as the stacks already are | 12,338 | 352 | none |
| 2 | No `snprintf` on the path: a small integer formatter, and codes the bench renders | 6,688 (about 6.2 kB net, estimate) | none | none |
| 3 | Open the flight log on the pad with its pages reserved, and append in flight without a page search | 4,250 for the open, about 3 kB more for the append (estimate) | none | none (DD-094 C6 already rules the pad open) |
| 4 | Size the sample ring to the samples that can wait (8, not 64), and keep only the newest row for the simulator's export | none | 8,848 | none |
| 5 | One estimator in flight, the other compared by replay on the bench | 1,482 | 4,060 | SNS-EST-07, DD-092 |

Precomputing every threshold in pressure once (SNS-EST-05's own wording) and
computing the descent speed once a sample change little code but take 20,000
to 30,000 cycles off each descent sample, about a quarter of today's work
(section 4.6). Lua and the network cannot run from RAM: the interpreter is
93.5 kB of code and the network 59.3 kB. Request (b) needs both exempt or
stopped in flight (section 6).

---

## 2. What runs from launch to landing, measured

### 2.1 The images measured

The three images were built from commit 48da822 in a scratch directory with
the toolchain and SDK of the `build-alfs-*` trees (GCC 14.2.1, Pico SDK
2.2.0, FreeRTOS-Kernel V11.3.1, alfs b5d946b). Their `.text`, `.data` and
`.bss` sizes equal those of the `build-alfs-mk1c`, `-mk1b` and `-mk1a` images
built from the same commit at 08:58 on 2026-10-18, byte for byte:

| Board | `.text` | `.rodata` | `.data` | `.bss` | Main SRAM above `.bss` |
|---|---|---|---|---|---|
| MK1C | 307,176 | 34,368 | 8,056 | 240,944 | 12,752 |
| MK1B | 295,392 | 32,840 | 8,052 | 240,796 | 12,900 |
| MK1A | 294,448 | 32,680 | 8,044 | 240,780 | 12,916 |

`.rodata` is 344 bytes larger than the `build-alfs` images': the scratch
directory's path is longer, and FreeRTOS's `configASSERT` stores the source
file's path (`src/rtos/FreeRTOSConfig.h:81`). About 840 bytes of the
read-only data counted on the path below are those paths; the boards' images
hold about 300 bytes less.

"Main SRAM above `.bss`" runs from the end of `.bss` to 0x20040000. It holds
the 2 kB `.heap` reservation, which newlib's allocator draws on for Lua's
number formatting. Each 4 kB scratch bank holds a core's 2 kB interrupt stack
at its top and nothing at its bottom, so 4 kB more is free there.

### 2.2 The method

The analysis follows `support/prove_core0.py`, which builds a call graph
from `objdump -d` and resolves veneers. The extension used here:

- **Nodes** are functions keyed by address from `objdump -t`; a function's
  size is its symbol size.
- **Edges** are `bl`/`blx` and `b` to another function (a tail call),
  veneers collapsed onto their targets.
- **Indirect calls** are followed only where named (below). A second,
  conservative closure also follows every function pointer found in a
  reachable function's literal pool or in a read-only object it references.
- **Read-only data** is every `.rodata` object or string whose address a
  reachable function's literal pool holds, followed through pointers stored
  in those objects. Strings are counted at their own length.

The roots, by where they run:

| Group | Roots |
|---|---|
| Flight task (core0) | `flight_task` (the loop, `src/main_hardware.c:200`), `dispatch_state`; the detectors of ASCENT, FALLING, DROGUE_DESCENT and CHUTE_DESCENT and the actions launch, armed, apogee and landing (indirect through `detectors[]` and `transitions[]`, `src/flight_states.c:36-71`); `hal_tasks_tick` and its ticks `pres_tick`, `buzzer_tick`, `gts_tick` (indirect through `async_task_t`); `flight_update_outputs`, `hal_pyro_update`, `pyro_update`, `flight_call_service`, `watchdog_update`, `lua_app_service`, `lua_app_dispatch`; both estimators' `reading`, `no_reading`, `pulse` and `estimate` (indirect through `estimator_lumped_vt` and `estimator_constacc_vt`); `real_pyro_ops` (indirect through `pyro_ch()`) |
| Interrupts | `xPortPendSVHandler`, `xPortSysTickHandler`, `prvFIFOInterruptHandler`, `hardware_alarm_irq_handler` and its callbacks `period_isr` and (MK1C) `pulse_end_isr`, `collector_alarm_isr`, `telem_uart_irq_handler` |
| Storage task (core1) | `storage_task`, `hal_storage_service`, `flight_storage_service`; the alfs driver's `fs_read` and `fs_prog` (indirect through `fs_driver`); `do_program_bytes` (indirect through `flash_op()`) |
| Kernel tasks | `prvIdleTask`, `prvPassiveIdleTask`, `prvTimerTask`, the lockout helpers' `helper_task` |

Calls that the code makes but that are not taken between launch and landing
are cut: the flight task's start-up (`period_start`, `watchdog_enable`), the
reboot and BOOTSEL exits, the image commit (5 s after boot), the self-test
save and `vfs_clean()` (both refused while the log runs), the pad record's
writes, the log's close (after landing), and the bench flight's mocks (USB
test mode). The pressure sensor's bring-up, every pre-launch state and the
LANDED state's detector, which runs after landing, are not roots.

Not counted: the net task (TinyUSB, lwIP, HTTP), the Lua task and its
interpreter, the USB interrupt (already in RAM), and the ROM, which costs
neither flash nor RAM. A restart in flight boots from flash (the resume of
FLT-BROWN-02); no layout of the flight code changes that.

### 2.3 Totals

| | MK1C | MK1B | MK1A |
|---|---|---|---|
| Code on the path | 65,992 | 61,814 | 61,456 |
| of which in flash | 62,872 | 58,694 | 58,336 |
| of which already in RAM | 3,120 | 3,120 | 3,120 |
| Functions | 404 | 364 | 360 |
| Code without the cuts | 70,254 | 66,348 | 65,990 |
| Code, conservative closure | 71,100 | 66,914 | 66,556 |
| Read-only data read | 3,286 | 3,718 | 2,899 |

The read-only data includes the scratch build's longer assert paths; on the
boards' images the path reads about 285 bytes less.

By group, on MK1C (groups share the kernel and helpers, so they overlap):

| Group | Code | of which in RAM |
|---|---|---|
| Flight task | 47,488 | 272 |
| Interrupts | 6,230 | 2,120 |
| Storage task | 15,938 | 624 |
| Kernel tasks | 4,632 | 142 |

The code already in RAM is the collector's handler (2,120 bytes,
SNS-COL-06), `flash_op_park`, `do_program_bytes`, the SDK's `flash_do_cmd`,
and the libm and libgcc helpers the SDK's linker script keeps in RAM.

### 2.4 By library and module

MK1C, code on the path:

| Library | Code | in flash |
|---|---|---|
| Application | 39,254 | 36,902 |
| Pico SDK | 11,510 | 11,014 |
| FreeRTOS | 7,888 | 7,888 |
| alfs | 6,378 | 6,378 |
| newlib | 690 | 690 |
| libm | 252 | 0 |
| libgcc | 20 | 0 |

The largest modules:

| Module | Code | What of it runs in flight |
|---|---|---|
| SDK `pico_printf` | 6,306 | `snprintf` for telemetry, estimator rows, Lua's status and MK1C's fire diagnostics; float formatting (`_ftoa`, `_etoa`, 2,596) comes with it |
| alfs `alfs_fs.c` | 5,844 | the log's open, remove, page search, append and sync |
| FreeRTOS `tasks.c` | 5,670 | context switch, tick, notify, critical sections, priority changes in `flash_op()` |
| `estimator_lumped.c` | 5,180 | the lumped EKF |
| `hal_common.c` | 4,128 | sample intake, log ring, telemetry UART, pyro and buzzer HAL |
| `buzzer.c` | 2,816 | one tick a period; silent in flight |
| `pressure_collector.c` | 2,240 | the collector (2,120 in RAM) |
| `pressure_processing.c` | 1,992 | gap and stuck watch, both estimators, ground reference, operator outputs |
| `fire_control.c` | 1,900 | the fire rules |
| MK1C `pyro_measure.c` | 1,804 | quiescent read, presence tracking |
| `atmosphere.c` | 1,788 | the 1976 standard atmosphere |
| `flight_descent.c` | 1,418 | descent detectors, pulse delivery |
| FreeRTOS `queue.c` | 1,302 | the filesystem mutex, the timer task's queue |
| `flight_states.c` | 1,262 | dispatch, sample intake, events |
| `pressure_estimator.c` | 1,212 | the constacc Kalman filter |
| `main_hardware.c` | 1,172 | the flight task's loop |
| `lua_app.c`, `lua_core1.c` | 924, 872 | the Lua service on the flight task |
| SDK `float_math.c` | 870 | `powf`, `log1pf` and friends, which compute in double |
| `flash_op.c` | 840 | the lockout (232 in RAM) |
| MK1C `pyro_sequence.c`, `arm_pump.c`, `pyro_board.c` | 708, 444, 420 | the firing sequence |
| SDK timer, PIO, IRQ | 664, 640, 604 | alarms; PIO and IRQ only through MK1C's lazy set-up (section 4.10) |
| SDK float and double shims | 586, 498 | the calls into the ROM's float and double routines |

### 2.5 The largest functions

MK1C, from the symbol table:

| Function | Bytes | Where | Note |
|---|---|---|---|
| `predict_to` | 3,064 | `src/estimator_lumped.c:202` | `step()` and the dense 4x4 `propagate_covariance()` unrolled at `-O3` |
| `_vsnprintf` | 3,040 | SDK `pico_printf` | |
| `buzzer_tick` | 2,668 | `src/buzzer.c:254` | every pattern's encoder inlined into the tick |
| `collector_alarm_isr` | 2,120 | `src/pressure_collector.c:176` | in RAM |
| `pp_feed_us` | 1,716 | `src/pressure_processing.c:243` | |
| `pres_tick` | 1,412 | `src/hal_common/hal_common.c:356` | MS5607 and BMP280 intake inlined |
| `_etoa`, `_ftoa` | 1,404, 1,192 | SDK `pico_printf` | float formatting, unused by the flight's formats |
| `reading` (lumped) | 1,372 | `src/estimator_lumped.c:308` | |
| `fire_control_step` | 1,344 | `src/fire_control.c:129` | |
| `pest_update` | 1,072 | `src/pressure_estimator.c:96` | |
| `flight_task` | 1,036 | `src/main_hardware.c:200` | |
| `find_end`, `link_pages` | 920, 864 | alfs | the log's open and page linking |
| `lua_app_service` | 900 | `src/lua/lua_app.c:334` | |
| `vTaskSwitchContext` | 752 | FreeRTOS | |
| `hal_storage_service` | 752 | `src/hal_common/hal_common.c:1382` | the log writer inlined |
| `tracking_step`, `read_quiescent`, `pulse_end_isr` | 656, 588, 492 | `boards/mk1c/pyro_measure.c` | |
| `run_fire_rules` | 616 | `src/flight_descent.c:87` | |

### 2.6 Read-only data on the path

MK1C, 3,286 bytes:

| What | Bytes |
|---|---|
| `printf`'s tables (`_vsnprintf`'s jump table, `pow10`) | 492 |
| FreeRTOS assert paths (`configASSERT`'s `__FILE__`) | 839 here, 554 on the boards' images |
| Format strings: telemetry, estimator rows, Lua status, MK1C fire diagnostics | about 650 |
| Event and fault names (`flight_event_name`, `pad_check_fault_name`), SDK panic strings | about 600 |
| `transitions[]`, `detectors[]` | 200 |
| Estimator vtables and table | 72 |
| `LAYERS` (atmosphere), alfs's CRC table, Lua's state names (`CSWTCH.40`), beep table | 252 |
| The rest: board name, file name, small constants | about 180 |

Constants loaded from literal pools sit inside the functions and are counted
as code.

### 2.7 RAM the flight path owns

MK1C, from the link map (`.bss` and `.data`):

| Object | Bytes | Where |
|---|---|---|
| Sample ring, 64 samples of 140 bytes | 8,960 | `pp.ring`, `src/pressure_processing.h:49` |
| Estimator snapshots for a stuck run, 3 of 192 | 576 | `pp.at_run_start` |
| Rest of `pp` (calibration, ground reference) | 416 | |
| Pressure trace, 256 records of 24 bytes | 6,144 | `src/pressure_trace.h:17` (SNS-PRES-13) |
| Log ring | 4,228 | `src/hal_common/hal_common.c:1155` |
| Log file buffer | 1,024 | `hal_common.c:1212` |
| Telemetry UART ring | 512 | `hal_common.c:684` |
| Sample intake (`pres`, with an unused batch ping-pong of 2x64) | 248 | `hal_common.c:200-220` |
| Flight context, of which 1,024 is `flight_buffer[64]` | 1,572 | `src/flight_states.h:54`, `:120` |
| Flight task stack | 4,096 | `src/rtos/rtos_tasks.h:36` |
| Storage task stack | 8,192 | `rtos_tasks.h:40` |
| Lockout helpers: two 1,024-byte stacks, records, the page command | 2,570 | `src/flash_op.c:29-37` |
| Kernel: two idle stacks, timer task stack, records, lists | 4,999 | FreeRTOS `tasks.c`, `timers.c`, `port.c` |
| alfs's block cache and scratch | 1,435 | `src/vfs_flash.c:12-32` |
| Buzzer patterns | 552 | `buzzer.c` (`BUZZER_MAX_PATTERN` 128) |
| Collector queue and part copy | 376 | `pressure_collector.c:27-40` |
| SDK float and double tables, alarm pool | 940 | |
| Estimators' state (lumped 160, constacc 72) | 232 | |
| Total | about 47,000 | |

The rest of RAM is the network (lwIP 44,049, HTTP's connections 41,904,
TinyUSB 5,477), Lua (arena 33,144, stack 16,384, script checker 16,384,
script buffer 8,192, rings 4,120), the net task's stack (8,192) and newlib's
allocator and locale (2,114).

### 2.8 What running the path from RAM needs today

| | MK1C | MK1B | MK1A |
|---|---|---|---|
| Code to move | 62,872 | 58,694 | 58,336 |
| Read-only data to move | 3,286 | 3,718 | 2,899 |
| Total | 66,158 | 62,412 | 61,235 |
| Free: main SRAM above `.bss` + scratch banks | 16,848 | 16,996 | 17,012 |
| Short by | 49,310 | 45,416 | 44,223 |

The conservative closure adds about 5 kB to each.

---

## 3. The algorithms as they are

### 3.1 One sample, from the sensor to the decision

1. **Collector** (`src/pressure_collector.c:176`, in RAM): an alarm-driven
   state machine commands a conversion, waits the part's worst case, reads,
   and queues each cycle's raw codes with the time of each conversion
   (SNS-COL-01..06, DD-093). The queue holds four cycles
   (`src/pressure_collector.h:28`).
2. **Intake** (`pres_tick`, `src/hal_common/hal_common.c:356`), once a
   period from `hal_tasks_tick()`: for each cycle, the MS5607's temperature is
   fitted on a line through the last four readings
   (`ms5607_temps_at`, `src/ms5607_driver.h:129`), the pressure is compensated to
   the second order (`ms5607_compensate_prom`, 64-bit integers), both go to
   the pressure trace, and the reading goes to `pp_feed_us()`.
3. **Processing** (`src/pressure_processing.c:243`): the gap and stuck
   watch, then **both** estimators take the reading
   (`feed_estimators`, `:163`), then `produce_sample` (`:214`) asks **both**
   for their estimate, feeds the ground reference, judges the rise above the
   pad (`note_rise`, `:200`) and converts pressure, rate and curvature into
   altitude, speed and acceleration for the operator (`for_the_operator`,
   `:207`). The sample (140 bytes) goes into a 64-slot ring.
4. **Dispatch** (`src/flight_states.c:97`): the state machine steps once for
   each sample waiting (FLT-RATE-06), up to 8 a period. Each detector copies
   the sample out (`pp_read`), records it (`flight_take_sample`, `:176`: a log
   row, both apogee detectors, an estimator row per estimator once a second,
   and a row in `flight_buffer`), then decides:
   - ascent (`src/flight_ascent.c:67`): thrust, maximum speed, arming height
     and speed, the peak and its altitude, apogee from the obeyed estimator's
     detector;
   - descent (`src/flight_descent.c:103-156`): the fire rules
     (`run_fire_rules`, `:87`, and `fire_control_step`,
     `src/fire_control.c:129`), the descent phase, landing.
5. **Outputs** (`src/flight_outputs.c:69`): the pyro back end's step, and the
   telemetry sentence once a second (`src/telemetry.c:55`).
6. **Storage task** (`src/hal_common/hal_common.c:1307`): opens the log on
   its first pass after launch (`:1328-1329`), writes the ring every 200 ms
   and syncs every second (FLT-LOG-06), through `vfs` and alfs to
   `flash_op_program_bytes()`.

### 3.2 Number formats and matrix sizes

- **Single-precision float** everywhere in the flight logic. The RP2040 has
  no FPU; each float operation is a call through the SDK's shim to the ROM
  (`rp2040-datasheet_2025-02-20.pdf`, section 2.8.3.2, Table 169, printed
  page 137): add 71 cycles, multiply 58, divide 71, square root 63, exp 524,
  ln 789, on average.
- **Double precision** inside the SDK's `powf` and `log1pf`:
  `powf(x, y)` is `(float)exp(log((double)x)*(double)y)` and `log1pf(x)` is
  `(float)log(1+(double)x)` (SDK 2.2.0 `pico_float/float_math.c:333`,
  `:341-343`). The ROM's double routines are fast (Table 170, printed page
  140: ln 428 cycles, exp 804, multiply 155), so a `powf` costs about 1,500
  cycles with its conversions and checks (estimate).
- **64-bit integers** in the MS5607 compensation (seven 64-bit multiplies)
  and the ground reference's mean.
- **lumped** (`src/estimator_lumped.c`): a 4-state EKF on height, speed,
  thrust parameter and log drag; the 4x4 covariance is propagated as a dense
  F·P·Fᵀ, two 4x4 products, 128 multiplies and 128 adds
  (`propagate_covariance`, `:133`), although two of F's four rows are
  diagonal (`:174-179`). One prediction step per reading (`STEP_MAX_S` 0.1 s),
  one scalar update. Its air is its own: the standard atmosphere's layers
  through the pad's pressure and the sensor's temperature (`air_from_pad`,
  `:84`).
- **constacc** (`src/pressure_estimator.c`): a 3-state Kalman filter in
  ln p with a 6-term covariance in closed form and a scalar update.

### 3.3 Work per sample

Operation counts from the disassembly (calls to the float shims, the ROM
functions and the atmosphere), priced at the ROM's cycles plus about 10
cycles of shim and call each. All cycle figures are estimates; the loop time
with both filters is not measured (`docs/lumped_parameter_filter.md`,
"Still owed").

| Stage | Float operations | Transcendental calls | Cycles (estimate) |
|---|---|---|---|
| Intake: temperature line, compensation, trace | about 50 | none | 5,000 |
| lumped prediction (`predict_to`) | 308 | 3 exp, 1 log1p, 1 pow | 28,400 |
| lumped update (`take`) | 102 | 1 log1p, 1 sqrt, 1 pow | 11,500 |
| lumped estimate | 20 | 2 exp, 1 log1p, 3 sqrt, 2 pow | 9,100 |
| constacc update and estimate | 103 | 1 ln, 1 exp, 4 sqrt | 9,500 |
| Rise and operator outputs | about 40 | 5 pow | 14,700 |
| Ascent detector, peak altitude each sample | about 25 | 2 pow | 8,300 |
| Descent: speed twice, AGL threshold, landing threshold | about 90 | 11 pow, 2 sqrt | 33,900 |
| **Total in ascent** | | | **about 86,000, 0.69 ms** |
| **Total in descent** | | | **about 112,000, 0.90 ms** |

At today's 53 samples a second the flight uses 4 to 5 % of core0; at 100 Hz
it would use 7 to 9 %. Running from flash adds cache misses on top: the XIP
cache is 16 kB (`rp2040-datasheet_2025-02-20.pdf`, section 2.6.3.1, PDF page
123), and the path alone is 63 kB.

The fixed work of a period (stage bookkeeping, pyro step, status copy, Lua
service, storage wake) is about 4,000 cycles. Once a second the telemetry
sentence, two `snprintf` calls when no event is queued, costs about 6,000
cycles, and the estimator rows, two height conversions and an `snprintf` for
each estimator, about 23,000 (estimates).

### 3.4 Work done twice or more

| What | Where | Cost per sample (estimate) |
|---|---|---|
| Two estimators fed, and asked for estimates, on every reading (SNS-EST-07) | `src/pressure_processing.c:170-175`, `:216-217` | 9,500 cycles; 1,426 bytes of code; 56 bytes per ring slot |
| The lumped air pressure evaluated four times per reading for nearly the same height | `src/estimator_lumped.c:130`, `:247`, `:331`, `:333` | 8,300 cycles |
| A dense covariance product over a matrix with two diagonal rows | `src/estimator_lumped.c:133-150` | 12,300 cycles |
| Height turned into pressure by the estimator, then back into height by the flight with another atmosphere | `estimate`, `src/estimator_lumped.c:333`; `for_the_operator`, `src/pressure_processing.c:209` | 5,800 cycles |
| Thresholds converted from height to pressure every sample, though SNS-EST-05 says once: rise 0.5 m, landing 30 m, each AGL trigger, each FALLEN trigger | `pressure_processing.c:201`, `src/landing_detector.c:18`, `src/fire_control.c:60`, `:63` | 5,700 cycles each |
| The descent speed computed twice, three `powf` each | `src/flight_descent.c:90`, `src/fire_control.c:136` | 7,100 cycles |
| The pad's air density recomputed inside every pad-air ratio | `src/atmosphere.c:87` | 1,500 cycles a call |
| The peak's altitude computed every ascent sample | `src/flight_ascent.c:26` | 5,800 cycles |
| The sample copied three times: ring slot cleared and filled, copied out, copied into `flight_buffer` | `pressure_processing.c:191`, `:266`; `flight_states.c:189` | about 400 cycles |

### 3.5 Text formatting in flight

| Caller | Formats | How often in flight |
|---|---|---|
| `send_sentence`, `send_event`, `send_state` (`src/telemetry.c:15-43`) | `$%s*%02X`, `PYRO,%u,...` (13 fields), `PYRO_APO`, `PYRO_FIRE`, `PYRO_LAND` | once a second, and at events (TEL-03, TEL-11) |
| `report`, `say_apogee` (`src/flight_estimators.c:24`, `:31`) | `%s cm=%ld cm/s=%ld explains=%d`, `%s APOGEE` | once a second per estimator (SNS-EST-07) |
| `report_fault` (`src/flight_outputs.c:42-47`) | `!FAULT %s...` | FAULT only, reachable from the outputs |
| `lua_app_service` (`src/lua/lua_app.c:364`, `:371`, `:380`) | status lines | when the script dies or is refused |
| MK1C `sequence_*` (`boards/mk1c/pyro_sequence.c:38`, `:155`), `boards/mk1c/pyro_board.c:64` | `!PYRO ...` diagnostics | at a fire |

Every one prints integers and strings only; none needs the float formatting
that `pico_printf` links. `src/flight_log.c:124-150` already holds a decimal
formatter without `printf`, for the CSV.

### 3.6 Rings and buffers

| Buffer | Size | Holds | Needed |
|---|---|---|---|
| Collector queue | 4 cycles + 1 building (SNS-COL-03) | cycles not yet taken | as is |
| Sample ring | 64 x 140 bytes | samples between intake and dispatch, in the same period | at most about 5 wait: the collector holds 4, and dispatch takes 8 a period (`src/flight_states.c:95`) |
| `by_estimator[ESTIMATORS_MAX]` in each sample | 3 x 28 bytes | every estimator's state, for its apogee detector and row | 2 carried; 1 if one is flown |
| `flight_buffer` | 64 x 16 bytes | the newest samples, for `flight_save_csv()`, which only the simulator calls (`sim/main_sim.c:57`) | the newest row, for an event's time (`newest_row`, `src/flight_states.c:172`) |
| Pressure trace | 256 x 24 bytes | SNS-PRES-13 | 256 records; 16 bytes each would carry the same fields (estimate) |
| Log ring | 4,096 bytes | 1.8 s of rows at 100 Hz | the launch hold-off (`hal_common.c:1155-1170`) |
| Telemetry queue | 12 events | TEL-11 | as is |
| Buzzer pattern | 128 steps | the longest beep-out | as is |
| `pres.back`, `pres.front` | 2 x 64 bytes | a batch interface no caller uses (`hal_pressure_fifo_get`, `hal_common.c:503`) | none |

### 3.7 Rates

- The collector runs free: the MS5607 converts a pressure and a temperature
  every 18.8 ms (53 a second), the BMP280 every 13.7 ms at 400 kHz and
  14.5 ms at MK1B's 100 kHz (DD-093).
- The flight task's period is 20 ms (DD-065, `src/loop_period.h:11`); each
  sample is a step of its own in the period it arrives (FLT-RATE-06).
- At 100 Hz the MS5607 answers every slot with the temperature converted one
  time in ten; the BMP280 at x4 does not, and at x2 it does (DD-063).

---

## 4. Options

### 4.0 Summary

Measured figures are code taken off the launch-to-landing path of MK1C,
from images built from 48da822 with the change applied, or from the call
graph with the named calls removed (removals only).

| # | Option | Code off the path | RAM back | Per-sample cycles | Requirement change |
|---|---|---|---|---|---|
| 4.1 | Flight path at `-Os` | 12,338 (measured) | 352 | +5 to 15 % (estimate) | none |
| 4.2 | No `snprintf` on the path | 6,688 removed (measured), about 500 added (estimate) | none | about 25,000 a second less (estimate) | none |
| 4.3 | Log opened on the pad, appended in flight without a search | 4,250 (measured) + about 3,000 (estimate) | none | none | none |
| 4.4 | One estimator in flight | 1,482 (measured) | 4,060 (measured) | 9,500 less (estimate) | SNS-EST-07, CFG table, DD-092 |
| 4.5 | Ring and buffer sizes | none | 8,848 (measured); trace 2,048 more (estimate) | 400 less (estimate) | none; SNS-PRES-13's format for the trace |
| 4.6 | Thresholds once, speed once | about 0 | about 32 added | 20,000 to 30,000 less (estimate) | none |
| 4.7 | Detectors merged into the estimator | about 500 (estimate) | small | 6,000 to 14,000 less (estimate) | none while comparisons stay in pressure (SNS-EST-05) |
| 4.8 | Fixed point; float without double | none to 1,000 (estimate) | none | none | none |
| 4.9 | Tables for the atmosphere | about 400 off, 256 to 512 of table on (estimate) | none | 1,400 a `powf` (estimate) | none |
| 4.10 | Cold paths out of hot functions | buzzer 2,724 (measured); MK1C set-up about 1,400 (estimate) | none | none | none |
| 4.11 | FreeRTOS: kept, trimmed | assert paths about 500 of data (estimate) | 1,000 to 4,000 of stacks (estimate) | none | none |
| 4.12 | 100 Hz sensor rates | none | none | twice the per-sample work a second | DD-066/DD-067 settings; FLT-LOG capacity |
| 4.13 | The ROM | already used; shims to RAM | none | none | none |
| 4.14 | Lua and network in flight | Lua service 2,936 (measured) | none | none | LUA-RUN-01, WEB-API-08 if stopped |

The first five combined, with the buzzer, measured on MK1C: 65,992 to
40,518 bytes of code (-39 %), 3,286 to 2,639 bytes of read-only data, and
10,136 bytes of RAM back (section 5.2).

### 4.1 Build the flight path for size

- **What changes:** the flight sources, the drivers, the log path, the HAL
  and the board library compile at `-Os`. The build already does so for
  FreeRTOS, lwIP, TinyUSB, Lua and the HTTP side (`CMakeLists.txt:236-240`,
  `:416-430`); only the flight path is at `-O3`.
- **Code:** measured. On the 35 flight objects, `.text` is 53,508 bytes at
  `-O3`, 43,056 at `-O2` (-20 %) and 34,384 at `-Os` (-36 %). On the path, the
  application's share goes from 39,254 to 26,776 bytes and the path from
  65,992 to 53,654 (-12,338). The MK1C image's `.text` goes from 307,176 to
  285,424 (-21,752); MK1B's from 295,392 to 274,968 (-20,424), which also
  matters for MK1B's 384 kB slot. `.data` loses 352 bytes, the collector's
  handler being smaller.
- **CPU:** every float operation is a call to the ROM at either level;
  `-O3`'s gain is unrolled loops and inlined calls, a few cycles on each
  70-cycle operation. Estimate: 5 to 15 % more cycles a sample, 0.03 to
  0.1 ms. 100 Hz holds.
- **Requirements:** none.
- **Risks:** timing moves a little everywhere; `prove_core0.py` and the
  collector's RAM proof (SNS-COL-06) must still pass, since `-Os` may turn a
  switch into a table or a struct copy into `memcpy` (DD-093 records both).

### 4.2 Take `snprintf` off the flight path

- **What changes:** telemetry sentences, MK1C's fire diagnostics and the
  estimator rows are built by a small formatter of unsigned, signed, two hex
  digits and strings, as `src/flight_log.c:124-150` already does for the CSV.
  The estimator rows become binary records (tag, height in cm, speed in cm/s,
  a flag) that the CSV renders as today's text. Lua's status lines become a
  status code and its numbers, rendered by `lua_app_status()` when the web
  side asks.
- **Code:** measured removal 6,688 bytes (59,304 left) and 510 bytes of
  read-only data; the formatter and the call sites add about 500 (estimate).
  `pico_printf` stays in the image for the HTTP side and Lua, so the image
  shrinks by only the formatting call sites.
- **CPU:** one `snprintf` with integers costs a few thousand cycles
  (estimate). At once a second the saving is small: about 4,000 cycles for
  the telemetry sentence, and about 23,000 for the estimator rows when they
  carry the estimator's pressure and rate and the CSV converts them
  (estimates).
- **Requirements:** none. TEL-01..12 keep their sentences byte for byte;
  SNS-EST-07's rows keep their content.
- **Risks:** a formatter is new code; `test/test_telemetry.c` and the flight
  suites that read telemetry check it against today's sentences.

### 4.3 Open the log on the pad; append in flight without a search

- **What changes:** the flight log's file is opened, and the last flight's
  removed, before launch, when OK to fly sounds, as DD-094's ruling C6 asks
  ("on the pad it is opened before OK to fly sounds"). The open reserves the
  flight's pages: `alfs_file_alloc()` takes a size and links that many pages
  so that "writing it later programs only erased flash" (alfs
  `alfs_fs.h:5-17`, `:118-127`); the firmware reserves none today
  (`src/vfs_flash.c:63`), so in flight the file grows a page at a time
  through alfs's free-page search and blank check (`grow`, `find_free`,
  `is_free`, alfs `alfs_fs.c:146-157`, `:361`, `:574`). alfs is not modified:
  the pad uses its reservation, and the flight appends to the reserved pages,
  through alfs or through an appender of the firmware's own that programs
  the pages the pad listed. A raw pre-erased area beside the filesystem is
  the other way (option W1 of `docs/flash_stall_options.md` on
  `docs-flash-stall-options`).
- **Code:** measured: without the open and remove in flight, the path is
  61,742 bytes (-4,250), and alfs's share falls from 6,378 to 2,928. An
  appender over given pages leaves about 500 bytes of the remaining 2.9 kB of
  alfs and the `vfs` layer's mutex calls (estimate: about 3,000 more off).
- **RAM:** none measured; `vfs_flash.c`'s copy buffers (512 bytes) are the
  bench's.
- **Request (b):** today's append reads flash in flight: alfs checks a page
  blank by reading it (`alfs_blank`, through `fs_read`,
  `src/fs_flash.c:37-41`) before taking it. A reservation made on the pad
  removes that search; whether alfs's append along reserved pages still
  reads a page header from flash decides between appending through alfs and
  an appender of the firmware's own (to be checked).
- **Requirements:** none. FLT-LOG-06 and FLT-LOG-07 keep their meaning;
  DD-100's "a write replaces a file whole" applies on the pad.
- **Risks:** an open on the pad that the board then abandons must leave
  nothing a later boot misreads. The pad record follows the same pattern:
  written on the pad (FLT-BROWN-01), cleared on the bench (FLT-BROWN-08).

### 4.4 One estimator in flight

- **What changes:** the build carries the lumped estimator alone
  (`src/estimator_table.c:13-16`, `ESTIMATORS_MAX` 1 in
  `src/estimator.h:50`). The constacc estimator is compared on the bench by
  replaying the flight log, which carries every raw reading for that purpose
  (DAT-08, `pyro_sim --replay`).
- **Code:** measured 1,482 bytes off the path (`pressure_estimator.c`,
  `estimator_constacc.c`, the per-estimator loops).
- **RAM:** measured 4,060 bytes of `.bss`: 56 bytes per sample in a 64-slot
  ring, one 192-byte snapshot, constacc's state.
- **CPU:** about 9,500 cycles a sample less (estimate).
- **Requirements:** SNS-EST-06 and SNS-EST-07 hold as written, since every
  estimator the build carries is fed and logged, but the configuration
  table's `estimator` row ("lumped or constacc") and DD-092 ("All Flown, One
  Obeyed") change. The in-flight comparison rows go; a replay gives the same
  rows from the log.
- **Risks:** a field change of estimator needs a new image rather than a
  `config.ini` line.

### 4.5 Ring and buffer sizes

- **What changes:** the sample ring holds 8 samples
  (`src/pressure_processing.h:49`), enough for the collector's 4 queued
  cycles and the 8 steps a period; `flight_buffer` keeps the newest row
  (`src/flight_states.h:54`), and the simulator keeps its own copy for its
  CSV export. Further, without measurement: the pressure trace's record packs
  to 16 bytes (32-bit stamp, 16-bit read lag, 24-bit codes, 24-bit
  pressure, kind), and the unused batch buffers in `pres` go.
- **RAM:** measured 8,848 bytes of `.bss` for the ring and `flight_buffer`
  (MK1C 240,944 to 232,096). The trace packing gives 2,048 more (estimate).
- **CPU:** the 140-byte clear and copies shrink with one estimator; about
  400 cycles a sample (estimate).
- **Requirements:** none for the ring and the buffer. The trace keeps 256
  records (SNS-PRES-13); its wire format `PTR1` and
  `support/pressure_trace.py` change.
- **Risks:** the ring's size becomes a coupling to `COLLECTOR_QUEUE` and
  `STEPS_A_LOOP_MAX`; a compile-time check states it. A full ring drops the
  oldest sample, as the collector does.

### 4.6 Thresholds once, speed once

- **What changes:** every height the flight compares is converted to
  pressure once, when its reference is fixed: the rise and landing pressures
  and each AGL trigger at launch, when the ground freezes; each FALLEN
  trigger at apogee, when the peak is final. The pad's air density is kept
  from launch. The descent speed is computed once a sample and passed to the
  fire rules and the descent phase. The peak's altitude is computed when
  telemetry, status or the apogee event asks for it.
- **Code:** about neutral (estimate): the conversion functions stay, called
  less.
- **RAM:** about 32 bytes.
- **CPU:** about 5,700 cycles for each threshold, 7,100 for the second
  descent speed, 1,500 for the pad density, 5,800 for the peak in ascent
  (estimates): 20,000 to 30,000 cycles a sample in descent.
- **Requirements:** none; SNS-EST-05 already says "converted to pressure
  once". PYR-MODE-06's guard keeps reading the state from before the charge.
- **Risks:** a resume (FLT-BROWN-06) must compute the thresholds from the
  recorded ground before its first decision.

### 4.7 Merge the detectors into the estimator

- **What changes:** the obeyed estimator reports its own height, speed and
  scale height beside the pressure, and the per-sample operator outputs
  (altitude, speed, acceleration) come from them rather than from converting
  the pressure back. The detectors themselves are small (launch 76 bytes,
  apogee 116, landing 240, descent phase 172); merging them buys little code.
  The work saved is the conversions of section 3.4.
- **Code:** about 500 bytes (estimate).
- **CPU:** 6,000 to 14,000 cycles a sample (estimate).
- **Requirements:** comparisons stay in pressure and rate (SNS-EST-05). The
  operator's altitude stays computed from the ground reference and the
  pressure (SNS-ALT-01), once a second for telemetry and status rather than
  every sample. The descent speed in pad air uses the 1976 standard
  atmosphere's temperature (FLT-AIR-01), so it keeps `atmosphere.c` or a
  table (4.9).
- **Risks:** the estimator's speed and the standard atmosphere's speed differ
  by the two air models' difference at height; the log keeps the pressure
  and the raw reading, so the CSV can render either.

### 4.8 Fixed point, and float without double

- **Fixed point:** no gain. A float operation is a 4-byte call to code in the
  ROM, which costs neither flash nor RAM; a Q-format multiply on a Cortex-M0+
  needs a 32x32-to-64 product, a call or about 20 instructions. The lumped
  covariance spans about 1e-7 to 1e4, beyond 32-bit fixed point without block
  scaling. Code would grow and accuracy would need a new study (estimate).
- **Float without double:** replacing `powf` and `log1pf` by float forms
  (`expf(y*logf(x))`, the ROM's float routines) takes most of the double
  shims (498 bytes on MK1C) and the double `log` and `exp` wrappers off the
  path at about the same cycles, since the ROM's double `ln` is faster than
  its float `ln`. With 4.6 and 4.9 few `powf` calls remain, so the gain is
  small: up to about 1,000 bytes (estimate).

### 4.9 Tables for the atmosphere

- **What changes:** the 1976 temperature and the scale height as a function
  of ln p, a 32- or 64-entry table interpolated linearly, replaces the
  `powf` inside `atmos_temperature_k` and `atmos_scale_height_m`
  (`src/atmosphere.c:63-82`).
- **Code and data:** about 400 bytes of code off, 256 to 512 bytes of table
  on (estimate); in (b) the table is RAM.
- **CPU:** about 1,400 cycles for each `powf` replaced (estimate).
- **Requirements:** FLT-AIR-01 and SNS-EST-05's 3 % hold if the table's error
  stays under 0.1 K (estimate, to be checked against `atmosphere.c`).

### 4.10 Cold paths out of hot functions

- **Buzzer:** `buzzer_tick` (2,668 bytes) inlines every pattern's encoder;
  in flight the buzzer is silent from launch (FLT-LAUNCH-05) to landing. An
  encoder outside the tick, or no buzzer tick from launch to landing, takes
  2,724 bytes off the path (measured).
- **MK1C's set-up in flight:** `arm_pump_start()` calls the guarded
  `arm_pump_init()` (`boards/mk1c/arm_pump.c:17-29`) and `tracking_step()`
  claims its alarm lazily (`boards/mk1c/pyro_measure.c:55-62`), which put
  the SDK's PIO loader, IRQ handler table and claim code on the path though
  they run once at boot: about 1,400 bytes (estimate, from the functions'
  sizes). Claiming both at boot takes them off.
- **FAULT and Lua status text:** see 4.2.
- **Requirements:** none.

### 4.11 FreeRTOS: kept, trimmed

- **What is used in flight:** 7,888 bytes of kernel code: the context switch,
  the tick (core1), task notifications from the period alarm and to the
  storage task, critical sections, the filesystem mutex, `flash_op()`'s
  priority changes, the idle tasks and the timer task.
- **The timer task stays** while `configSUPPORT_PICO_SYNC_INTEROP` is 1
  (`src/rtos/FreeRTOSConfig.h`): the port signals an SDK lock's release from
  an interrupt with `xEventGroupSetBitsFromISR`, which defers through the
  timer task (`portable/ThirdParty/GCC/RP2040/port.c:1051`). Built with
  `configUSE_TIMERS` 0, the port fails to compile there.
- **Trims:** `configASSERT` without the file path (about 500 bytes of data
  on the boards, and `strrchr`/`strchr`, 240 bytes of code); stacks sized
  from their high-water marks on a bench flight: the storage task's 8,192
  bytes, the timer task's 2,048 and the idle tasks' 1,024 each (1,000 to
  4,000 bytes back, estimate).
- **Without the scheduler in flight:** a flight run from the period
  interrupt, or a scheduler stopped at launch, would take the kernel off the
  path. FreeRTOS SMP has no supported way to stop and restart the scheduler,
  the net and Lua tasks share core1, and the flight's guarantees rest on
  DD-073's model and `prove_core0.py`'s proof of it. Not recommended.
- **Requirements:** none (DD-080 makes FreeRTOS a design fact, not a
  requirement).

### 4.12 100 Hz

**Sensors.** A pressure every 10 ms, each with its temperature (DD-066):

| Setting | Conversion time, max | Cycle (estimate) | Rate | Noise a sample | Noise per second of samples |
|---|---|---|---|---|---|
| MS5607, OSR 4096 both (today) | 9.04 ms each | 18.87 ms (measured, DD-093) | 53/s | 2.4 Pa | 0.33 Pa |
| MS5607, OSR 2048 both | 4.54 ms each | 9.9 ms | 101/s | 3.6 Pa | 0.36 Pa |
| MS5607, OSR 4096 pressure, OSR 256 temperature | 9.04 + 0.60 ms | 10.4 ms | 96/s | 2.4 Pa, and about 1.5 Pa through the temperature | 0.29 Pa |
| BMP280, forced x4 (today) | 13.3 ms | 13.7 / 14.5 ms (measured, DD-093) | 73 / 69/s | 2.1 Pa | 0.25 Pa |
| BMP280, forced x2 | 8.7 ms | 9.1 ms at 400 kHz, 9.9 ms at 100 kHz | 110 / 101/s | 2.6 Pa | 0.26 Pa |

Sources: `MS5607-02BA03_2017-06.pdf` page 3 (conversion times) and page 4
(RMS resolution); `BST-BMP280-DS001-26_2021-10.pdf` Table 13, page 18
(measurement time) and Table 8, page 15 (noise). Cycle times add the
collector's transfers and margins as measured today (about 0.33 ms a
conversion at 400 kHz). Noise per second is the noise a sample over the
square root of the rate (estimate). The temperature's share: its RMS
resolution is 0.012 °C at OSR 256 and 0.003 °C at OSR 2048 (page 4); a
temperature error moves the compensated pressure about 240 Pa per °C
(`src/ms5607_driver.h:106`), and the line through four temperatures
(`MS5607_TEMPS`, `:109`) about halves the noise (estimate): about 1.5 Pa at
OSR 256, under 0.5 Pa at OSR 2048.

- **The MS5607 at OSR 2048 for both conversions** is the only setting that
  reaches 100 Hz on that part with a temperature beside every pressure. On a
  battery the boards measure 2.3 to 2.6 Pa, the datasheet's 2.4 Pa (DD-094;
  `boards/mk1c/THEORY_OF_OPERATION.md`, the sensor row); on USB power alone
  they measure 6 to 9 Pa, which is the supply's. The datasheet's figures are
  therefore the ones expected to scale with oversampling (estimate).
- **The BMP280 at x2** reaches 100 Hz on MK1A; on MK1B's 100 kHz bus it is
  just at 101 a second.
- **The period:** the 20 ms period steps two samples a period (FLT-RATE-06);
  a 10 ms period halves the decision latency, doubles the period's fixed
  work (about 0.2 % of core0, estimate), and lets MK1C's arm pump use shorter
  words again (DD-065). Lua's tick budget would need halving to keep its
  share.
- **CPU:** today's code at 100 Hz needs 7 to 9 % of core0 (section 3.3);
  the smaller version under 5 % (section 5.3).
- **The log:** every sample is a row (FLT-LOG-07, DD-101). At 100 Hz the
  rows double: about 10 to 11 pages a second against 5.9 (DD-101), and the
  capacity halves: MK1C about 50 minutes, MK1B about 6 minutes, MK1B-8MB
  about 27 (estimates; DAT-09). A 12-byte row (time delta, 24-bit pressure
  and raw reading, temperature, state, thrust and event packed), with the
  altitude rendered at download from the pressure and the header's ground,
  keeps today's page rate (estimate). DAT-02 is a CSV requirement and holds.
- **Discarded conversions:** a page program beside a conversion discards it
  (DD-068, SNS-PRES-14). Twice the pages a second against conversions half as
  long: about the same share discarded (estimate).
- **Requirements:** FLT-RATE-01 (at least 50 a second) holds. DD-066,
  DD-067 and DD-093's settings change; SNS-EST-03 and SNS-EST-04's tests fly
  at 1.2 and 9 Pa a sample and cover 3.6 Pa.

### 4.13 The ROM

The RP2040's ROM holds the single- and double-precision arithmetic and
scientific functions (Tables 169 and 170), the float conversions, `memcpy`
and `memset`, and the bit operations. The firmware already calls all of them
through the SDK's shims; code in the ROM costs no flash and no RAM and is
not fetched through XIP. For request (b) only the shims count: on MK1C's path
the float shims (586 bytes), the double shims (498), `float_math.c` (870),
the divider (380), the 64-bit multiply (52) and the memory shims (24).
The SDK's `PICO_FLOAT_IN_RAM`, `PICO_DOUBLE_IN_RAM`, `PICO_DIVIDER_IN_RAM`,
`PICO_INT64_OPS_IN_RAM` and `PICO_MEM_IN_RAM` place the assembly shims in
RAM; `float_math.c` is C and goes wherever the flight's own placement puts
it.

### 4.14 Lua and the network in flight

- **Lua:** the interpreter and its bindings are 93,492 bytes of code; its
  RAM is already about 78 kB. The interpreter cannot run from RAM. On the
  flight task, the Lua service (`lua_app_service`, `lua_app_dispatch`) is
  2,936 bytes of the path (measured removal), most of it status text (4.2).
- **The network:** TinyUSB, lwIP and HTTP are 59,302 bytes of code. In
  flight on battery no host is attached; the net task still runs its loop.
- **Request (b):** either both run from flash on core1 in flight, which
  request (b) would have to exempt as it exempts the USB test mode, or both
  are suspended from launch to landing, which changes LUA-RUN-01 ("the
  enabled script shall always run ... in flight") and WEB-API-08 ("the web
  API and USB shall stay live in flight").

---

## 5. A smaller architecture

### 5.1 Shape

The same tasks, in one pass per sample:

1. **Collector**, unchanged, in RAM (SNS-COL-06), at OSR 2048 on the MS5607
   or x2 on the BMP280.
2. **Intake and estimate:** each cycle is compensated and fed to the one
   flown estimator, lumped. The estimator evaluates its air once a reading,
   propagates the covariance with F's structure, and reports pressure, rate,
   curvature, height, speed and its scale height. No ring of samples: the
   flight steps on each sample as it is produced, or through an 8-slot ring
   if intake and dispatch stay in separate stages.
3. **Decisions in pressure** against thresholds converted once (4.6): rise
   and launch on the pad; arming height, landing height and AGL triggers at
   launch; FALLEN triggers at apogee. The descent speed in pad air computed
   once a sample, with the pad's density kept.
4. **Records as numbers:** the log row and the estimator row are binary
   records; telemetry is formatted by the integer formatter once a second;
   status text is rendered by the bench side.
5. **Storage in flight:** the storage task appends the ring's bytes into the
   pages the pad reserved, and syncs once a second; no search, no blank
   check, no open, no remove, no flash read.
6. **Cold work stays in flash:** pad checks and presence tests, beep encoding,
   the file system's open, close and cleanup, configuration, status text,
   MK1C's PIO and alarm set-up (at boot).
7. **The kernel as it is**, trimmed (4.11).

### 5.2 Size

Measured on images built from 48da822 with `-Os` on the flight path, one
estimator, an 8-slot ring and a one-row `flight_buffer`, and on their call
graphs without the log's open in flight, without `snprintf` and without the
buzzer tick:

| | MK1C | MK1B | MK1A |
|---|---|---|---|
| Code on the path, today | 65,992 | 61,814 | 61,456 |
| `-Os`, one estimator, ring 8, one row | 52,198 | 49,438 | 49,130 |
| and no open, no `snprintf`, no buzzer in flight | 40,518 | 37,666 | 37,358 |
| of which already in RAM | 2,772 | 2,772 | 2,772 |
| Read-only data | 2,639 | 3,067 | 2,705 |
| Added back: formatter, appender, thresholds, split-out cold paths (estimate) | about 1,500 | about 1,500 | about 1,500 |
| **To place in RAM for request (b)** | **about 41,900** | **about 39,500** | **about 38,800** |
| Without the Lua service on the flight task | 37,822 code, 2,039 data | | |
| RAM back (`.bss` and `.data`, measured) | 10,136 | 10,136 | 10,132 |
| Image `.text` with `-Os`, one estimator, ring 8, one row | 283,744 (-23,432) | 273,288 (-22,104) | 272,560 (-21,888) |

What remains on MK1C's smaller path: FreeRTOS 7,604, the application
21,582, the SDK 4,822 (float shims and `float_math.c`, timer, MK1C's PIO and
IRQ set-up), alfs 2,928, newlib 614. With MK1C's set-up claimed at boot
(4.10) and an appender over given pages (4.3), about 4 kB more goes
(estimate).

### 5.3 Rate

| Stage | Today (estimate) | Smaller (estimate) |
|---|---|---|
| Intake | 5,000 | 5,000 |
| lumped | 49,000 | 28,400 |
| constacc | 9,500 | none |
| Rise and operator outputs | 14,700 | 700 |
| Ascent detector | 8,300 | 900 |
| Descent rules and landing | 33,900 | 12,000 |
| **Ascent, a sample** | **86,000 (0.69 ms)** | **35,000 (0.28 ms)** |
| **Descent, a sample** | **112,000 (0.90 ms)** | **46,000 (0.37 ms)** |
| **core0 at 100 Hz** | **7 to 9 %** | **3.5 to 4.6 %** |

At `-Os` add 5 to 15 % (4.1). Running from RAM removes the XIP cache's misses
from both columns. The decision latency is the period's: 20 ms, or 10 ms
with a 10 ms period.

### 5.4 What it means for running the flight from RAM

| MK1C | Today's path | Smaller path |
|---|---|---|
| To place in RAM | 66,158 | about 41,900 |
| Free today (main SRAM above `.bss` + scratch banks) | 16,848 | 16,848 |
| + RAM back from the smaller path (measured) | | 10,296 |
| = Free, lwIP kept | 16,848 | 27,144 |
| Shortfall, lwIP kept | 49,310 | about 14,800 |
| + smallest_tcp (38,480, measured on the spike) | 55,328 | 65,624 |
| Margin with smallest_tcp | -10,830 | **about +23,700** |

The free space grows by 10,296 bytes where `.bss` and `.data` shrink by
10,136: the start of `.bss` moves with its alignment. MK1B and MK1A come out
alike (about +26 kB with smallest_tcp). Without smallest_tcp, the last 15 kB
would have to come from Lua's script checker (16,384 bytes of static
scratch, used only when a script is saved), the pressure trace (6,144) or
the storage stack after its high-water mark is measured.

Two further conditions hold for request (b) whatever the size:

- **Lua and the network** stay in flash on core1 (exempt) or stop from launch
  to landing (4.14).
- **A restart in flight** boots from flash; the resume path (FLT-BROWN-02)
  is outside any RAM placement.

---

## 6. Decisions for the user

1. **Lua and the net task in flight:** exempt from request (b), running from
   flash on core1, or suspended from launch to landing (LUA-RUN-01,
   WEB-API-08 change)?
2. **One estimator in flight:** fly lumped alone and compare constacc by
   replay (SNS-EST-07's rows from the replay; DD-092 and the configuration
   table change)?
3. **`-Os` for the flight path:** take 12.3 kB off the path and 21.7 kB off
   the image for 5 to 15 % more cycles a sample (estimate)?
4. **Text off the flight path:** an integer formatter for telemetry and
   MK1C's diagnostics, binary estimator rows, and Lua status rendered by the
   bench side?
5. **The log opened on the pad** (DD-094 C6) with the flight's pages
   reserved there (`alfs_file_alloc` with a size), and appended in flight
   through alfs or an appender of the firmware's own, or a raw area beside
   the filesystem?
6. **100 Hz on the MS5607:** OSR 2048 for both conversions (101 a second,
   DD-066 kept), or OSR 4096 pressure with OSR 256 temperature (96 a second)?
   On the BMP280: x2 (DD-067 changes)?
7. **The period:** keep 20 ms with two samples a period, or 10 ms?
8. **The log at 100 Hz:** keep 22-byte rows and halve the capacity (MK1B
   about 6 minutes, DAT-09), or a 12-byte row with the altitude rendered at
   download?
9. **Buffers:** the sample ring at 8 and the simulator's 64-row buffer moved
   to the simulator; the pressure trace packed to 16-byte records, or not
   written from launch to landing?
10. **Thresholds once** (4.6) and **the atmosphere as a table** (4.9): adopt
    both for the per-sample work, or only 4.6?

---

## Appendix: reproducing the figures

- **Images:** commit 48da822 configured with `-DPYRO_BOARD=<board>
  -DCMAKE_BUILD_TYPE=Release`, `FETCHCONTENT_SOURCE_DIR_*` pointing at the
  pinned dependencies, target `pyro_fw_c`.
- **Variants,** each a copy of the tree with these edits, alone or combined:
  `os`: `set_source_files_properties(... COMPILE_OPTIONS "-Os")` on
  `PYRO_FLIGHT_SOURCES`, the drivers, the log path, `rtos_tasks.c`,
  `storage_task.c` and the Lua service, and `target_compile_options(... -Os)`
  on `pyro_hal_common` and `pyro_board`; `one`: `estimator_constacc_vt`
  removed from `src/estimator_table.c`, `ESTIMATORS_MAX` 1; `ring8`:
  `PP_RING_SIZE` 8; `buf1`: `FLIGHT_BUF_SIZE` 1.
- **Call graph:** `support/prove_core0.py`'s `build_graph()` extended with
  tail calls (`b` to another function), the roots and cuts of section 2.2,
  and the named indirections. The removals of sections 4 and 5 cut every
  call to `__wrap_snprintf`; the calls from `hal_storage_service` to
  `vfs_remove` and `vfs_open`; and the roots `buzzer_tick`, `lua_app_service`
  and `lua_app_dispatch`.
- **RAM:** `.bss` and `.data` input sections from the link map, by object.
- **Per-object optimisation levels:** each flight object compiled with its
  own command from `compile_commands.json`, at `-O3`, `-O2` and `-Os`.
