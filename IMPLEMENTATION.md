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
PAD_IDLE        → ASCENT           on SEVT_LAUNCH           (100 ft and 5 m/s)
ASCENT          → ASCENT           on SEVT_ARMED
ASCENT          → FALLING          on SEVT_APOGEE           (the filtered pressure past its minimum)
FALLING         → DROGUE_DESCENT / CHUTE_DESCENT  on SEVT_DROGUE / SEVT_CHUTE (a settled descent rate)
DROGUE_DESCENT  → CHUTE_DESCENT on SEVT_CHUTE; → FALLING on SEVT_FREEFALL
FALLING / DROGUE_DESCENT / CHUTE_DESCENT → LANDED on SEVT_LANDING
```

### State Transition Criteria

**PAD_IDLE → ASCENT:** 100 ft above the ground reference while climbing at 5 m/s or more, on the filtered state, with no hold (`launch_detected()`, FLT-LAUNCH-02, FLT-LAUNCH-07), and no USB host attached unless test mode is on (USB-01, USB-08). T+0 is back-dated to the start of the rise (FLT-LAUNCH-03).

**ASCENT → FALLING:** the obeyed estimator seen climbing and then seen falling, each by more than three times its rate's uncertainty, with its model explaining the readings throughout; a fall whose climb was not seen that way must last 2 s (FLT-APO-07, DD-092). There is no hold and no Mach flag: a port error near Mach 1 is readings the model does not explain (SNS-EST-08). Every estimator the build carries is flown and logged; `estimator` in config.ini names the one obeyed (`src/estimator.h`, SNS-EST-06, SNS-EST-07).

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
FreeRTOS SMP on both cores (DD-073, `src/rtos/rtos_tasks.h`). `main()` in `src/main_hardware.c` brings the board up on core0 and starts the scheduler. `flight_task()` runs alone at P on core0, woken every 20 ms (`src/loop_period.h`, DD-065) by an alarm on the hardware timer. In order: `hal_tasks_tick()` (the pressure sensor first, so the MS5607 is commanded at a steady offset); `flight_call_service()` (changes the net task handed over); `dispatch_state()`; `flight_update_outputs()`; Lua's service and its tick request; then a notification to the storage task. The net task (`src/net_task.c`: TinyUSB, lwIP, HTTP's transport and work units, DD-061), the Lua task (`src/lua/lua_core1.c`) and the storage task (`src/storage_task.c`: `hal_storage_service()` and `flight_storage_service()`) share core1 at P. Every flash program or erase runs through `flash_op()` under a lockout that parks the other core (DD-074). The flight task waits only for its period (FLT-RT-01). The watchdog is twice `PYRO_LOOP_WORST_MS`.

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
| `src/pressure_estimator.c`, `src/pressure_processing.c`, `src/atmosphere.c` | The filtered state, samples and the ground reference, the standard atmosphere |
| `src/fire_control.c`, `src/fire_plan.c` | The fire rules; the plan from the configuration and the board |
| `src/flight_resume.c` | The pad record and the resume decision |
| `src/telemetry.c` | $PYRO NMEA and its queued events |
| `src/config.c`, `src/config_fields.h` | `config.ini`: X-macro table, parser, serializer |
| `src/buzzer.c`, `src/beep_codes.c`, `src/beep_store.c` | Non-blocking beep sequencer; outcomes and personalities |
| `src/hal.h` | Hardware abstraction interface |
| `src/hal_common/hal_common.c` | Pico SDK HAL implementation, shared by every board |
| `src/pressure_collector.c` | The free-running collector: conversions, the queue of four, bus recovery (DD-093) |
| `src/ms5607_driver.c`, `src/bmp280_driver.c` | Each sensor's detection, description for the collector, and arithmetic |
| `src/flight_log.c`, `src/log_plan.c` | Binary flight log, CSV rendering, logging plans |
| `src/http_server.c`, `src/http_conn.c`, `src/http_work.c` | HTTP routes, connections, work units |
| `src/net_glue.c`, `src/net_txq.c` | USB network, lwIP, mDNS, held frames |
| `src/ground_test_seq.c`, `src/ground_test_switch.c` | The ground test procedure and its switch |
| `src/flight_sim.c`, `src/bench_flight.c` | The bench flight's profile and its hold on the channels |
| `src/pin_assign.c`, `src/pin_store.c`, `src/pad_claim.c` | `pins.ini` and pad ownership |
| `src/lua/` | The script |
| `src/main_hardware.c` | Hardware main loop |
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
0x10A000  LittleFS             984 KB
```
MK1A and MK1C (16 MB): two 4076 KB slots from 0x00A000 and 0x405000, and 8 MB of littlefs from 0x800000. Each board's `board.cmake` sets the geometry.

### HTTP API
| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | JSON device state |
| GET | `/api/net` | lwIP pools, TCP connections, transport refusals (WEB-API-13) |
| GET | `/api/pressure/trace` | The last 256 conversions, binary (SNS-PRES-13) |
| GET | `/api/log/space` | Room for the next flight's log (WEB-API-12) |
| GET | `/api/flight.csv` | The flight log, rendered as CSV |
| POST | `/api/flight/erase` | Erase the flight log |
| GET/POST | `/api/config` | Config INI file; a POST is merged and stored, and takes effect at the next start (CFG-10) |
| GET | `/api/limits` | The board's pyro timing defaults and ranges, what is in force, the sensor's range (PYR-BOARD-03) |
| GET, POST | `/api/sim`, `/api/sim/flight`, `/api/sim/stop` | The bench flight, where the board has one (SIM-01..04) |
| GET/POST | `/api/pins` | `pins.ini` |
| GET | `/api/pins/caps` | The board's capability table and its vocabulary |
| GET/POST | `/api/beeps` | Beep personalities; `POST /api/beeps/play` auditions one |
| GET/POST | `/api/lua/script` | The Lua program; `POST /api/lua/check` validates one against the configured resources, `GET /api/lua/console` reads the script's output |
| POST | `/api/test_mode/on`, `/api/test_mode/off` | Test mode (USB-08) |
| POST | `/api/serial` | The board's MAC, into `serial.txt` |
| POST | `/api/reboot` | Restart device |
| POST | `/api/ota` | Firmware update |
| POST | `/www/<file>` | Upload a web file |
| GET | `/<path>` | Any littlefs file |

The API is live in flight; a request that needs a file gets 423 while the flight log holds the filesystem (DD-058, WEB-API-10).

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
The flight log (`flight_log.bin` in littlefs) opens at launch and closes at LANDED. It holds binary records (`src/flight_log.h`); `/api/flight.csv` renders them as CSV with `flog_csv_read()` (DD-062). The flight code hands every sample to `hal_log_sample()`, and `log_plan_take()` keeps what `log_rate` asks for: a row a second, that plus every sample within 1 s of an event, or every sample (FLT-LOG-07, DD-064). Records go to a 4 KB RAM buffer.

**Flash stalls the CPU.** The RP2040 executes from flash via XIP, and a sector erase (46-73 ms measured, DD-035) stops both cores fetching from it. So:
- nothing is written until the RAM buffer has filled once or 2 s have passed, which carries the log through the launch-shock window (DD-084, DD-027);
- after that the buffer is written, and the file synced once a second, only in core0's flash window between core1 work units (FLT-LOG-06, DD-035), so a flight that never lands keeps its record;
- from launch until the log lets go, every other file request is refused (`HAL_FS_LOCKED`, HTTP 423, DD-058).

The flight context also keeps a ring of the last 64 samples and events; `flight_save_csv()` exports it for the simulator.

### Pressure
Each reading is stamped at the moment it describes, not when the loop reads it (SNS-PRES-08): the pressure collector, one interrupt state machine running from RAM, commands each conversion, waits out the part's worst case on an alarm, reads, and queues the raw codes with the time of each conversion's middle (DD-093). The sensor task takes the queue every loop and compensates. A conversion a flash erase or program ran beside is discarded (DD-068). A failed transfer is counted by cause, and after three in a row the collector clears the bus and resets the part (SNS-COL-04, SNS-COL-05).

The raw readings go straight into one Kalman filter (`pest_update()` in `src/pressure_estimator.c`, DD-085): its state is ln(p / p_ref), its rate and its acceleration, with the sensor's noise measured from the readings. A reading more than six standard deviations from the prediction is skipped unless it is the third in a row. There is no median, no low-pass and no reseed. Every flight comparison is made in pressure, with heights and speeds the operator set converted once against the pad and the 1976 standard atmosphere (`src/atmosphere.c`, SNS-EST-05).

Altitude is computed for people, relative to the pad, and is not clamped: a point below the pad reads negative and no ceiling is applied (SNS-ALT-01). Each sensor's rated range and its height for proper operation are in `src/pressure_sensor.h`, and each board declares them (SNS-MAX-01).

### Ground Test
By the switch alone (DD-087). The procedure is `gt_seq_step()` in `src/ground_test_seq.c`, which owns the schedule and fires on it with or without a buzzer; a fire is delivered on command (GND-TEST-13). `gt_switch_step()` reads a switch to ground, or across two pads by driving one and requiring the other to follow both ways; the driven pad may be the buzzer's, pulsed only for the read (GND-TEST-12, `docs/ground_test_on_buzzer_pad.md`).

### Simulation
The `sim/` directory contains a WASM-compilable flight computer black box and a shared physics engine. See `sim/README.md` for architecture and integration guide.

### Testing
- Host suites: see `test/README.md`; `scripts/run_host_tests.sh` runs every one, locally and in CI
- Playwright web UI tests in 3 mock server modes, and the browser demo flown from power-on to LANDED
- `TRACEABILITY.md` is written by `support/trace_matrix.py` from the tests' own citations
- `support/structure_check.py`, `support/trace_check.py`, `support/wait_check.py` and `support/prove_core0.py` in CI
- cppcheck with MISRA addon, clang-format, pmccabe complexity in CI
