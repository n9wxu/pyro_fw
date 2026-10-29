# Flight Controller Implementation

## Architecture

### Event-Driven State Machine
The flight computer uses a transition table that defines every possible state change:

```
{ from_state, event, to_state, action }
```

Three components:
- **Detectors**: One per state, in a table indexed by state. Read sensors, do per-tick work, return an event or SEVT_NONE.
- **Actions**: One-time side effects at transitions (logging, buzzer, pyro firing).
- **Engine**: `dispatch_state()` calls the detector → looks up (from, event) in the table → calls the action → returns the new state. A state out of range returns PAD_IDLE.

### Transition Table
The table is `transitions[]` in `src/flight_states.c`; `docs/flight_states.md` has every state, the fire rules and the reasons. BOOT_SENSOR, FAULT and GROUND_TEST are numbered after LANDED, so no recorded state number moves.
```
BOOT_SETTLE     → BOOT_SENSOR      on SEVT_TIMER            (2.5 s settle)
BOOT_SENSOR     → BOOT_CONTINUITY  on SEVT_DONE             (sensor answered, storage usable, no flight in progress)
BOOT_SENSOR     → ASCENT / FALLING on SEVT_RESUME_*         (a flight in progress, FLT-BROWN-02)
BOOT_SENSOR     → FAULT            on SEVT_FAULT
BOOT_CONTINUITY → BOOT_CALIBRATE   on SEVT_DONE
BOOT_CONTINUITY → GROUND_TEST      on SEVT_GROUND_TEST      (switch held at start-up, GND-TEST-05)
BOOT_CALIBRATE  → PAD_IDLE         on SEVT_CAL_DONE         (10 readings)
BOOT_CALIBRATE  → FAULT            on SEVT_FAULT            (no samples in 10 s)
PAD_IDLE        → ASCENT           on SEVT_LAUNCH           (15 m)
ASCENT          → ASCENT           on SEVT_ARMED
ASCENT          → FALLING          on SEVT_APOGEE           (the filtered altitude past its top)
FALLING         → DROGUE_DESCENT / CHUTE_DESCENT  on SEVT_DROGUE / SEVT_CHUTE (a settled descent rate)
DROGUE_DESCENT  → CHUTE_DESCENT on SEVT_CHUTE; → FALLING on SEVT_FREEFALL
FALLING / DROGUE_DESCENT / CHUTE_DESCENT → LANDED on SEVT_LANDING
```

### State Transition Criteria

**PAD_IDLE → ASCENT:** 15 m above the ground reference while climbing at 15 m/s or more, on the filtered state, with no hold (`launch_detected()`, FLT-LAUNCH-02, FLT-LAUNCH-07), and no USB host attached unless test mode is on (USB-01, USB-08). T+0 is back-dated to the start of the rise (FLT-LAUNCH-03).

**ASCENT → FALLING:** the obeyed estimator seen climbing and then seen falling, each by more than three times its rate's uncertainty, with its model explaining the readings throughout; a fall whose climb was not seen that way must last 2 s (FLT-APO-07, DD-092). A port error near Mach 1 is readings the model does not explain, and nothing is decided on them (SNS-EST-08). Every estimator the build carries is flown and logged; `estimator` in config.ini names the one obeyed (`src/estimator.h`, SNS-EST-06, SNS-EST-07).

**Descent:** the phase is read from the descent rate settling in a band, never from a firing command (FLT-DESC-01). The fire rules are `fire_control_step()` in `src/fire_control.c`: each channel's own trigger, re-fire, and the emergency fire (DD-082).

**→ LANDED:** under 2 m/s for 1 s within 30 m of the ground, from any descent state; or the landing timeout, `landing_timeout` (60 s) after apogee, and still.

**BOOT_CONTINUITY → GROUND_TEST:** `hal_ground_test_asserted()` held through the last 500 ms of BOOT_SETTLE (GND-TEST-05, DD-087). A board resuming a flight never takes it.

### Hardware Abstraction Layer
The flight software (`src/flight_sources.txt`) contains no platform-specific code and no conditional compilation; `support/structure_check.py` holds it to that. All hardware interaction goes through `hal.h`, which is also the seam every host test mocks (HAL-05):

| HAL Function | Purpose |
|---|---|
| `hal_time_ms()` | Current time |
| `hal_pressure_init()`, `hal_pressure_sensor()` | Sensor bring-up and its result; samples reach the pressure layer from `hal_tasks_tick()` |
| `hal_pyro_init()`, `hal_pyro_sample()`, `hal_pyro_get()`, `hal_pyro_fire()`, `hal_pyro_update()`, `hal_pyro_fault()`, `hal_pyro_limits()` | Pyro channels, and what the board permits of their timing |
| `hal_reset_cause()` | Why the processor started |
| `hal_buzzer_init()`, `hal_buzzer_tone_on()`, `hal_buzzer_tone_off()` | Buzzer |
| `hal_telemetry_send()` | UART output |
| `hal_fs_open()`, `hal_fs_write()`, `hal_fs_close()`, `hal_fs_read_file()`, `hal_fs_write_file()`, `hal_fs_read_cached()` | Filesystem |
| `hal_config_load()`, `hal_config_save()` | `config.ini` |
| `hal_log_start()`, `hal_log_sample()`, `hal_log_stop()` | Flight log |
| `hal_ground_test_asserted()` | Ground test switch |
| `hal_tasks_tick()` | The autonomous tasks: pressure sensor, buzzer, ground test switch |

Three implementations: `src/hal_common/hal_common.c` with each board's files in `boards/<name>/` (Pico), `test/hal_test.c` (mocks), `boards/sim/hal_sim.c` (simulation).

### Tasks
FreeRTOS SMP on both cores (DD-073, `src/rtos/rtos_tasks.h`). `main()` in `src/main_hardware.c` brings the board up on core0 and starts the scheduler. `flight_task()` runs alone at P on core0, woken every 10 ms (`src/loop_period.h`, DD-104) by an alarm on the hardware timer. In order: the start of core1's frame (`core1_frame_begin()`, which wakes the storage task); `hal_tasks_tick()`; `flight_call_service()` (changes other tasks handed over); `dispatch_state()`; `flight_update_outputs()`; Lua's service and its tick request. The net task (`src/net_task.c`: TinyUSB, smallest_tcp, HTTP's transport and work units, DD-073), the Lua task (`src/lua/lua_core1.c`) and the storage task (`src/storage_task.c`: `hal_storage_service()` and `flight_storage_service()`) share core1 at P. Core1 runs in the period's frame (`src/core1_frame.h`, DD-104): the storage task writes the flight log from the period's start for up to 3 ms and then gives the frame up; Lua's tick starts then, or at 3 ms, and runs for up to 6.5 ms, ending by 9.5 ms. On the ground every flash program or erase runs through `flash_op()` under a lockout that parks the other core (DD-074). The log writer's go through `flash_op_slot_program()` and `flash_op_slot_erase()`, which in flight run each in pieces inside the writer's slot, on core1 alone, by the chip's datasheet figures (`src/flash_slot.c`, `src/flash_timing.c`); a piece that waits for a slot is started at the slot's start by an alarm interrupt on core1, and an erase is suspended at the slot's end and resumed in the next. In flight an erase comes only when the log finds no erased page (`VFS_NOSPC`), one sector a time, and only on a part whose erase ends in pieces (`flash_op_can_erase()`; not BY25Q64ES). An operation the chip holds suspended is finished at every start (`flash_chip_finish_suspended()`) and before the first ground operation after a flight that gave one up (FLT-LOG-11). The flight task waits only for its period (FLT-RT-01). The watchdog is twice `PYRO_LOOP_WORST_MS`.

### The flight path in RAM
From the head of the first period that starts airborne with the flight log running until landing, core0 runs only from RAM (FLT-RT-02, FLT-RT-03, DD-104): the flight task, its interrupt handlers, the idle tasks, the kernel paths they take, and every read-only byte they read. `support/flight_ram_link.py` runs every firmware link: it links once, has `support/prove_flight_ram.py` find the path in that image, and links again with the path's sections renamed `.time_critical.flight.*`, which the linker script copies to RAM; `<image>.flight_ram.txt` beside the image lists them. CI proves each image with `prove_flight_ram.py --strict`. The flight task enters flight (`flash_op_flight_begin()`) at the head of its period and leaves it in `hal_log_stop()` at landing; meanwhile every flash operation but the writer's is refused, a mailbox call whose function lives in flash is refused, and a reset asked for over USB waits. `/api/status` reports `flash_in_flight`, the writer's part (`flash_part`), the frame (`frame`: the writer's longest hold, its overruns, Lua ticks given no time) and the pieces (`slots`: run, suspended, slots waited, operations refused in flight, the longest).

Pads are owned once: `pin_store_claim_pads()` gives each pad one owner, and `pyro_release_claim()` and `lua_iface_publish()` install operations only for pads they could claim (DD-020).

### Pyro health and what a pulse records
- Health is read once a second on the pad and decides the announcement only. No reading withholds a fire (PYR-HEALTH-01, DD-081).
- `hal_pyro_fault(channel)`: MK1B reads its AP2192 FLAG pins; MK1C reports its bus faults; MK1A has no fault output.
- Every pulse is recorded: whether the board energised it (`hal_pyro_is_firing()` straight after `hal_pyro_fire()`), a fault during it (EVT_PYRO1_FAULT, EVT_PYRO2_FAULT), and whether the channel read open afterwards (EVT_PYRO1_NOPEN, EVT_PYRO2_NOPEN). Nothing depends on those records (PYR-FIRE-01, PYR-VERIFY-01).
- Each board declares the faults it can detect, its pulse and its limits in its `THEORY_OF_OPERATION.md`, "What this board declares" (BRD-01).

### Key Source Files
| File | Purpose |
|---|---|
| `src/flight_sources.txt` | The flight software's file list, one responsibility a file (`docs/flight_states.md` has the table) |
| `src/flight_states.c`, `src/flight_states.h` | The transition table, the dispatcher, the context |
| `src/estimator.h`, `src/estimator_table.c`, `src/estimator_constacc.c` | The estimators: one interface, the table the build carries, the constant-acceleration filter (DD-092, DD-115) |
| `src/altitude_filter.c`, `src/pressure_processing.c`, `src/atmosphere.c` | The constant-acceleration Kalman filter, samples and the ground reference, the standard atmosphere and the pad's air |
| `src/fire_control.c`, `src/fire_plan.c` | The fire rules; the plan from the configuration and the board |
| `src/flight_resume.c` | The pad record and the resume decision |
| `src/telemetry.c` | $PYRO NMEA and its queued events |
| `src/config.c`, `src/config_fields.h` | `config.ini`: X-macro table, parser, serializer |
| `src/buzzer.c`, `src/beep_codes.c`, `src/beep_store.c` | Non-blocking beep sequencer; outcomes and personalities |
| `src/hal.h` | Hardware abstraction interface |
| `src/hal_common/hal_common.c` | Pico SDK HAL implementation, shared by every board |
| `src/pressure_collector.c` | The free-running collector: conversions, the queue of four, bus recovery (DD-093) |
| `src/ms5607_driver.c`, `src/bmp280_driver.c` | Each sensor's detection, description for the collector, and arithmetic |
| `src/flight_log.c` | Binary flight log, CSV rendering |
| `src/vfs.c`, `src/vfs_flash.c`, `src/fs_flash.c` | Every file through one interface, the SD card first where there is one; alfs on the flash, its driver and the store's lock (DD-076, DD-100) |
| `src/rtos/rtos_tasks.c`, `src/net_task.c`, `src/storage_task.c` | The tasks, their cores and priorities; the net task; the storage task (DD-073) |
| `src/flash_op.c`, `src/flash_slot.c`, `src/flash_timing.c` | Flash operations: the ground's lockout, the flight's pieces in the writer's slots, the chips' figures (DD-074, DD-104) |
| `src/core1_frame.h`, `src/rtos/core1_frame.c` | Core1's frame: the writer's slot, then Lua's (DD-104) |
| `src/small_fmt.c` | The flight path's text formatter, integers only, the C library's output |
| `src/http_server.c`, `src/http_conn.c`, `src/http_work.c` | HTTP routes, connections, work units |
| `src/net_glue.c`, `src/net_txq.c`, `src/http_transport.c` | USB network over smallest_tcp, DHCP, mDNS, held frames; HTTP's links (DD-102) |
| `src/ground_test_seq.c`, `src/ground_test_switch.c` | The ground test procedure and its switch |
| `src/flight_sim.c`, `src/bench_flight.c` | The bench flight's profile and its hold on the channels |
| `src/pin_assign.c`, `src/pin_store.c`, `src/pad_claim.c` | `pins.ini` and pad ownership |
| `src/lua/` | The script |
| `src/main_hardware.c` | `main()` and the flight task |
| `sim/main_sim.c` | Simulation black box (WASM target) |
| `boards/sim/hal_sim.c` | Simulation HAL |
| `sim/physics.c` | Shared physics engine |
| `sim/sim_cli.c` | CLI physics driver |

### Flash Layout
MK1B (2 MB):
```
0x000000  Bootloader           36 KB
0x009000  Info block             4 KB
0x00A000  App Slot A           512 KB
0x08A000  App Slot B           512 KB
0x10A000  Files (alfs)         984 KB
```
MK1B-8MB (8 MB): two 2028 KB slots from 0x00A000 and 0x205000, and 4 MB of files from 0x400000. MK1A, MK1C and MK1C-SD (16 MB): two 4076 KB slots from 0x00A000 and 0x405000, and 8 MB of files from 0x800000. Each board's `board.cmake` sets the geometry.

### HTTP API
| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | JSON device state |
| GET | `/api/net` | TCP links by state, retransmissions, transport counts, frames (WEB-API-13) |
| GET | `/api/pressure/trace` | The last 256 conversions, binary (SNS-PRES-13) |
| GET | `/api/log/space` | Room for the next flight's log (WEB-API-12) |
| GET | `/api/fs` | The flash store's space and each file, the cleanup's state and pace (WEB-API-19) |
| GET | `/api/fs/scan?sector=K&count=N` | How many pages of each of up to 128 sectors read erased (WEB-API-20) |
| GET | `/api/flight.csv` | The flight log, rendered as CSV |
| POST | `/api/flight/erase` | Erase the flight log |
| POST | `/api/delete/<path>` | Delete a file from every store; the board's identity and pad record refused (WEB-API-17) |
| GET | `/api/flight/live?since=N` | The flight log held for the host in test mode, from byte N (WEB-API-16) |
| GET/POST | `/api/config` | Config INI file; a POST is merged and stored, and takes effect at the next start (CFG-10) |
| GET | `/api/limits` | The board's pyro timing defaults and ranges, what is in force, the sensor's range (PYR-BOARD-03) |
| GET, POST | `/api/sim`, `/api/sim/flight`, `/api/sim/stop` | The bench flight, where the board has one (SIM-01..04) |
| GET, POST | `/api/sd`, `/api/sd/bench`, `/api/sd/init`, `/api/sd/idle`, `/api/hr`, `/api/hr/start`, `/api/hr/stop` | MK1C-SD's card and its high-rate log (DD-075, DD-077) |
| GET/POST | `/api/pins` | `pins.ini` |
| GET | `/api/pins/caps` | The board's capability table and its vocabulary |
| GET/POST | `/api/beeps` | Beep personalities; `POST /api/beeps/play` auditions one |
| GET/POST | `/api/lua/script` | The Lua program; `POST /api/lua/check` validates one against the configured resources, `GET /api/lua/console` reads the script's output |
| POST | `/api/test_mode/on[?log=host]`, `/api/test_mode/off` | Test mode, and the log to the host in place of the flash (USB-08, USB-09) |
| POST | `/api/serial` | The board's MAC, into `serial.txt` |
| POST | `/api/reboot` | Restart device |
| POST | `/api/ota` | Firmware update |
| POST | `/www/<file>` | Upload a web file |
| GET | `/<path>` | Any file on the flash |

The API is live in flight; a request that needs a file gets 423 while the flight log holds the filesystem (DD-058, WEB-API-10). A route marked `writes` in `src/http_routes.h` gets 503 with `Retry-After: 1` while the cleanup has sectors left to erase (WEB-API-21, DD-108).

### Telemetry ($PYRO NMEA)
```
$PYRO,seq,state,thrust,alt_cm,vel_cms,maxalt_cm,press_pa,time_ms,flags,p1adc,p2adc,0,0*XX\r\n
```
One message a second in PAD_IDLE, in flight and in LANDED (TEL-03). State codes 0-5: PAD_IDLE, ASCENT, FALLING, DROGUE_DESCENT, CHUTE_DESCENT, LANDED. XOR checksum. An apogee, a fire or a landing is queued and carried ahead of the next message's state sentence (TEL-11). The port accepts no commands; another format is a script's, on serial pins assigned to it (TEL-12).

### Buzzer
- **Pad:** one of four outcomes (general_fault, check_pyro_1, check_pyro_2, ok_to_fly; one at a time, in that priority) under the active personality, repeating every 5 s until launch by default; the diagnosis is on `/api/status`
- **USB:** one double chirp on attach, and no announcement while a host is attached (USB-02, USB-03)
- **Landing:** Altitude beep-out in configured units, repeats forever
- **Ground test:** its own alert, countdowns, tone and all-clear (GND-TEST-06, GND-TEST-07)

### Flight Log
The flight log (`flight_log.bin` on the flash) opens in the first period airborne and closes at LANDED, written only into erased pages (DD-100). On the ground the storage task's cleanup erases while no flight log is open: on USB with test mode off a sector at each of its passes, otherwise a sector a second (USB-10); in flight the writer erases one only when the log finds no erased page (FLT-LOG-10). It holds binary records (`src/flight_log.h`); `/api/flight.csv` renders them as CSV with `flog_csv_read()` (DD-062). The flight code hands every sample to `hal_log_sample()`, and the log keeps each one (FLT-LOG-07, DD-101). Records go to a 4 KB RAM buffer. In test mode set with `?log=host`, the storage task hands them to a 4 KB ring in RAM (`src/log_live.c`) in place of the file, read by offset at `/api/flight/live` (USB-09, DD-103).

**Flash stalls whatever fetches from it.** The RP2040 executes from flash via XIP, and a program or an erase stops it answering. So:
- nothing is written until the RAM buffer has filled once or 2 s have passed, which carries the log through the launch-shock window (DD-084, DD-027);
- after that the buffer is written every 200 ms, or as soon as it is nearly full, and the file synced once a second, in the writer's slots (FLT-LOG-06, FLT-LOG-09), so a flight that never lands keeps its record; core0 runs from RAM meanwhile and does not stop;
- from launch until the log lets go, every other file request is refused (`HAL_FS_LOCKED`, HTTP 423, DD-058).

The flight context also keeps a ring of the last 64 samples and events; `flight_save_csv()` exports it for the simulator.

### Pressure
Each reading is stamped at the moment it describes, not when the loop reads it (SNS-PRES-08): the pressure collector, one interrupt state machine running from RAM, commands each conversion, waits out the part's worst case on an alarm, reads, and queues the raw codes with the time of each conversion's middle (DD-093). The sensor task takes the queue every loop and compensates. A conversion a flash erase or program ran beside is used like any other, and counted (DD-106). A failed transfer is counted by cause, and after three in a row the collector clears the bus and resets the part (SNS-COL-04, SNS-COL-05).

Each reading becomes an altitude, the 1976 standard atmosphere's height of its pressure, and goes straight into every estimator the build carries (`src/estimator.h`, DD-092, DD-114); the flight obeys the one `estimator` in config.ini names. `constacc`, the one carried (DD-115), is a Kalman filter on the altitude, its speed and its acceleration, with the sensor's noise measured from the readings in pascals (`af_update()` in `src/altitude_filter.c`, DD-085). It sets aside a reading more than six standard deviations from its prediction for the first 50 ms of its run (SNS-EST-02). `src/pressure_processing.c` turns the obeyed estimate into a height above the pad and true speeds in the pad's air, the standard atmosphere offset by the pad's temperature (`src/atmosphere.c`), and every flight comparison is made on them (SNS-EST-05).

A height is relative to the pad and is not clamped: a point below the pad reads negative and no ceiling is applied (SNS-ALT-01). Each sensor's rated range and its height for proper operation are in `src/pressure_sensor.h`, and each board declares them (SNS-MAX-01).

### Ground Test
By the switch alone (DD-087). The procedure is `gt_seq_step()` in `src/ground_test_seq.c`, which owns the schedule and fires on it with or without a buzzer; a fire is delivered on command (GND-TEST-13). `gt_switch_step()` reads a switch to ground, or across two pads by driving one and requiring the other to follow both ways; the driven pad may be the buzzer's, pulsed only for the read (GND-TEST-12, `docs/ground_test_on_buzzer_pad.md`).

### Simulation
The `sim/` directory contains a WASM-compilable flight computer black box and a shared physics engine. See `sim/README.md` for architecture and integration guide.

### Testing
- Host suites: see `test/README.md`; `scripts/run_host_tests.sh` runs every one, locally and in CI
- Playwright web UI tests in 3 mock server modes, and the browser demo flown from power-on to LANDED
- `TRACEABILITY.md` is written by `support/trace_matrix.py` from the tests' own citations
- `support/structure_check.py`, `support/trace_check.py`, `support/wait_check.py`, `support/prove_core0.py` and `support/prove_flight_ram.py --strict` in CI
- clang-format and pmccabe complexity as gates in CI; cppcheck with the MISRA addon reports and does not fail the build
