# Tests

Host-compiled tests for the Pyro flight computers. Every suite builds with the
build machine's C compiler, not the ARM toolchain, from any configured build
directory. Each test is black box and traces to a requirement (TST-01,
CODE-08): it drives published interfaces only, and names the requirement in its
own name (`test_FLT_APO_01_...`), in a `[FLT-APO-01]` citation, or in its
suite's `Verifies [...]` header. `TRACEABILITY.md` is written from those
citations by `support/trace_matrix.py`; this file does not repeat it.

What stands in for the hardware depends on the suite:
- `hal_test.c` implements `hal.h` on mock state for the flight software suites
  (HAL-05): a sensor with noise, glitches, dropouts and a stuck reading, pyro
  channels that record every pulse, an in-memory filesystem, a clock that
  moves only when the test moves it;
- `board_harness.c` powers that flight software on and steps its loop as
  `main_hardware.c` does. Whatever a test stores before the first tick is what
  the board finds at power-on;
- `flight_run.c` flies a whole flight beside its truth: the firmware through
  the harness, a rocket through `sim/mach_plant.c`, and what each did;
- `test/fake_sdk/` stands in for the Pico SDK under a board file;
- `sim/hw/` and `sim/plant/` run MK1C's real backend against a model of its
  board.

A module with no platform links alone, so a module that reached for live state
would not link.

## Running

```bash
export CI_BUILD=1               # without it the image reports VERSION+local
scripts/run_host_tests.sh       # every suite CI runs: build, build-mk1c, build-mk1a
cmake --build build --target flight_profile_tests   # one suite
```

Each target builds its binary into the build directory and runs it. A suite
that compiles with `-I boards/<PYRO_BOARD>` sees the selected board's
`board_pins.h`, so a board-specific assumption shows only when it runs in more
than one build: the boot, pad and recorded-flight suites run on all three.

| Target | Source | Covers |
|--------|--------|--------|
| `flight_boot_tests` | `test_flight_boot.c` | start-up: the checks before the pad, and the terminal fault |
| `flight_pad_tests` | `test_flight_pad.c` | the pad: the announcement and its priority, pyro health, the ground reference, launch, the pad record, USB |
| `flight_profile_tests` | `test_flight_profiles.c` | whole flights: every pyro mode, apogee from 60 m to 45 km, descent and landing, the log and its replay, storage in flight, a late and a stalled loop |
| `fire_rule_tests` | `test_fire_rules.c` | re-fire and emergency fire with canopies that fail, board limits, thin air |
| `fire_control_tests` | `test_fire_control.c` | the fire rules alone (`fire_control.c`), a sample at a time |
| `resume_tests` | `test_resume.c` | a restart in flight: the decision, and what a resumed flight does |
| `estimator_tests` | `test_estimator.c` | the filtered state: tracking, noise, bad readings, AGL from three pad elevations |
| `estimators_tests` | `test_estimators.c` | each estimator the build carries, through its interface alone, on the same readings |
| `apogee_detector_tests` | `test_apogee_detector.c` | the apogee rule alone (`apogee_detected()`) |
| `telemetry_tests` | `test_telemetry.c` | the downlink of a whole flight, read as a ground station reads it |
| `mach_tests` | `test_mach.c` | the Mach flag: supersonic flights, port errors, failed sensors (below) |
| `recorded_flight_tests` | `test_recorded_flight.c` | the OpenRocket export in `test_data/`, pad to landed (TST-02) |
| `chamber_flight_tests` | `test_chamber_flight.c` | a pressure chamber run in `test_data/`, and a fall too fast for the model (FLT-APO-08..11) |
| `altos_flight_tests` | `test_altos_flights.c` | fifteen real flights from AltOS in `test_data/altos/`: launch, and apogee against AltOS's own (TST-02) |
| `atmosphere_tests` | `test_atmosphere.c` | the 1976 standard atmosphere (`atmosphere.c`) |
| `physics_tests` | `test_physics.c` | the simulators' rocket (`sim/physics.c`) and its atmosphere against the published tables |
| `launch_detector_tests` | `test_launch_detector.c` | the launch trigger alone (`launch_detected()`): 15 m above the pad |
| `ground_test_tests` | `test_ground_test.c` | ground test mode through the flight software (DD-087) |
| `ground_test_seq_tests` | `test_ground_test_seq.c` | its schedule and switch, a loop at a time |
| `buzzer_tests` | `test_buzzer.c` | the pattern player driven through `hal_tasks_tick()`, and the beep store |
| `beep_tests` | `test_beep_codes.c` | the four outcomes, the personalities and their file format |
| `beep_out_tests` | `test_beep_out.c` | the altitude beep-out, heard from the buzzer pin |
| `config_tests` | `test_config.c` | round trip, defaults, parser, serializer, merge |
| `config_persistence_tests` | `test_config_persistence.c` | the stored file, and that a change waits for the next start (CFG-10) |
| `pin_caps_tests` | `test_pin_caps.c` | MK1A's pin capability table, whatever board is selected |
| `pin_assign_tests` | `test_pin_assign.c` | pin assignment rules: the release matrix, the common, the buzzer and ground test pads |
| `lua_tests` | `test_lua.c` | the Lua sandbox and its limits, on `boards/sim/lua_platform_sim.c` |
| `http_tests` | `test_http.c` | the HTTP engine as a byte stream (below) |
| `http_transport_tests` | `test_http_transport.c` | HTTP over smallest_tcp, against the real stack at both ends (DD-102) |
| `http_work_tests` | `test_http_work.c` | who runs each HTTP work unit, and within what budget (DD-073) |
| `status_json_tests` | `test_status_json.c` | `/api/status` rendered from a snapshot, `status_json.c` linked alone |
| `fs_json_tests` | `test_fs_json.c` | `/api/fs`'s rendering: the flash store and each of its files |
| `vfs_flash_tests` | `test_vfs_flash.c` | the board's files, `vfs_flash.c` over alfs, on a RAM flash that keeps NOR's rules and counts its erases (DD-100) |
| `loop_period_tests` | `test_loop_period.c` | the periods the flight task's alarm skips when it fires late, each a missed period |
| `flash_slot_tests` | `test_flash_slot.c` | the log writer's flash operations inside their slots, against a chip that keeps its datasheet's suspend rules |
| `flash_chip_tests` | `test_flash_chip.c` | what a flash chip says of itself: capacity, unique ID length, whether the image's flash map fits |
| `small_fmt_tests` | `test_small_fmt.c` | the flight path's formatter, `small_fmt.c`, against the C library's (FLT-RT-02) |
| `log_live_tests` | `test_log_live.c` | the flight log held for the host in test mode (USB-09) |
| `net_stats_tests` | `test_net_stats.c` | `/api/net`'s rendering |
| `net_txq_tests` | `test_net_txq.c` | the USB network's held frames against a fake endpoint (WEB-NET-05, WEB-NET-07) |
| `mac_random_tests` | `test_mac_random.c` | the MAC from the flash chip's whole unique ID, and an assigned one in `/serial.txt` (DD-099, WEB-NET-06) |
| `hr_log_tests` | `test_hr_log.c` | the high-rate log on a fake card and IMU, its tasks run by hand (`test/fake_rtos`, DD-077) |
| `flight_sim_tests` | `test_flight_sim.c` | the bench flight: its atmosphere and profile, canopies that fail, its hold on the channels (DD-078) |
| `flight_log_tests` | `test_flight_log.c` | the flight log's binary records and the CSV they render as (DD-062) |
| `pressure_trace_tests` | `test_pressure_trace.c` | the ring `/api/pressure/trace` reads (DD-063) |
| `collector_tests` | `test_collector.c` | the pressure collector's interrupt state machine on a fake bus, clock and part (below) |
| `ms5607_tests` | `test_ms5607.c` | the MS5607's arithmetic: compensation, the temperature carried to each pressure, the PROM's CRC |
| `bmp280_tests` | `test_bmp280.c` | the BMP280's detection and arithmetic on a fake part |
| `sensor_bringup_tests` | `test_sensor_bringup.c` | every board's sensor bring-up |
| `board_pyro_tests` | `test_board_pyro_mk1b.c`, `test_board_pyro_mk1a.c` | MK1B's pyro backend on the fake SDK, as fitted and with the netlist's part, then MK1A's (`board_pyro_mk1a_tests`) |
| `plant_tests` | `test_plant.c` | the three board plant models |
| `board_pyro_mk1c_tests` | `test_board_pyro_mk1c.c` | MK1C's pyro backend on its modelled board; builds `plant_tests` first |
| `board_selftest_tests` | `test_board_selftest.c` | whether an image belongs on this board |

`sim` is the simulator, not a test: see `sim/README.md`.

`test/web/run_web_tests.sh` runs the Playwright suite against the mock server
in all three modes, then `test_sim.spec.js` against `docs/` served as GitHub
Pages serves it (`static_server.js`): the browser demo's WASM build, flown from
power-on to LANDED. Run it from `test/web` after `npm install` and
`npx playwright install chromium`.

`scripts/run_host_tests.sh` also runs the checks that read the source rather
than run it: `support/structure_check.py` (requirements that are properties of
the source, such as HAL-01, TEL-12 and CODE-09), `support/trace_check.py`
(every cited requirement exists, every requirement has a row),
`support/trace_matrix.py --check` (`TRACEABILITY.md` is up to date),
`support/wait_check.py` (no sleeps) and `scripts/format_check.sh`; and
`sim/study/lumped_port_check.py`, which replays the study's flights through
`src/estimator_lumped.c` (`test/estimator_replay.c`) against the study's filter.

## Flights against their truth

`flight_run.h`'s `fly()` takes a rocket, a pad and a `flight_conditions_t`, and
returns what the firmware did beside what the rocket did. All zero is a clean
flight on the default configuration. The conditions cover what each channel's
charge does (a canopy at a rate, on which pulse it lights, or never), a canopy
lost on the way down, thin air, faulted channels, a board that energises
nothing, a board's own pyro limits, static ports that misreport near Mach 1, a
bay charge, sensor dropouts, a stuck sensor, glitches and noise, a restart in
flight, a loop that runs late, and a processor held by other work.

The plant (`sim/mach_plant.c`) has an atmosphere set by the pad's temperature
and elevation through three layers, the speed of sound, a rocket whose drag
rises through Mach 1, and static ports whose error is a Mach-dependent
fraction of the dynamic pressure. `ROCKETS[]` names nine flights from a 60 m
hop to 45 km. It is separate from `sim/physics.c`, the WASM module's rocket.

The sensor model in `mocks.h` adds seeded Gaussian noise and truncates to
whole pascals, and every reading is fed whatever its value (SNS-PRES-06). The
flights fly at 1.2 Pa, below either part's datasheet figure, and at 9 Pa
(`test/board_harness.h`). `support/noise_baseline.py <board-ip>` measures a
board's real noise.

`collector_tests` runs the pressure collector's interrupt state machine
(`src/pressure_collector.c`, DD-093) on a fake bus, clock and part, with each
sensor's own description: `test/collector_bus.h` stands in for
`src/hal_common/collector_bus.h`. It covers each conversion in turn, reads
that wait out the part's worst case, the stamp at the middle of a conversion,
a late task, failed and endless transfers, the bus cleared and the part reset,
and a conversion a flash operation ran beside marked (DD-106).

`test_DAT_08_a_log_replays_to_the_same_events` flies a flight, reads back its
log, and replays the log's readings through `sim/replay.c`. The same code is
`pyro_sim --replay <flight_log.csv>` (build `sim` with `PYRO_BOARD=sim`), for
real flights (DAT-08).

## Board files on the host (DD-053)

`test/fake_sdk/` stands in for the Pico SDK calls a board file makes: pins,
the ADC, I2C with fake devices (`fake_i2c.c`), and a clock that moves only
when the test moves it. Its sleeps and busy-waits fail the test that reaches
them, and so does a blocking I2C transfer to a device holding SCL low, which
on the SDK never returns (DD-069). The `_timeout_us` forms time out on a held
bus, or when the bound is shorter than the transfer at the bus's rate.
- `board_pyro_tests` runs MK1B's `pyro_board.c` against it twice: as fitted,
  where the AP2192A holds the sense nodes at ground and every channel reports
  ready, and with the netlist's part, where continuity is read as MK1A reads
  it (DD-059), its settle waited out across loops, never inside one. Then
  MK1A's own, `board_pyro_mk1a_tests`.
- `sensor_bringup_tests` runs every board's sensor bring-up —
  `src/pressure_single_sensor.c`, or MK1B's own `pressure_board.c` — each
  built against its own package (mk1a, mk1b, mk1c, reference), with fake
  sensors that refuse a transfer during their reset.
- `bmp280_tests` runs the BMP280 driver on MK1A's pins against a fake part:
  detection leaves it asleep, the datasheet's example compensates as printed,
  and every code compensates in order. Its conversions are the collector's
  (`collector_tests`).

## MK1C's firing bus as measured (DD-054, DD-055, DD-056)

`plant_tests` holds `sim/plant/plant_mk1c.c` to the bench MK1C: U9's reverse
path and the bias sources' Schottky, fitted to the board's ADC and a scope,
from `boards/mk1c/pyro_sense.h`. `board_pyro_mk1c_tests` runs MK1C's real
backend — `boards/mk1c/pyro_board.c`, `pyro_measure.c`, `pyro_faults.c`,
`pyro_sequence.c` and `arm_pump.c` — against that model through `sim/hw/`,
whose PIO stand-in runs the pump's FIFO and bursts:
- between fires, a fitted match reads present and an absent one open, a
  shorted bus or high side latches, one bad tracking reading does not, and
  the only stimulus is the tracking test's bus bias;
- a fire arms the bus, closes the gate on the measured bus or at its
  deadline, stops the pump at the gate, and verifies with the next tracking
  test, on 2S and 1S; no reading withholds a fire (an open channel, no
  presence test yet, a latched fault, a low pack, a bus already live); a
  bus that will not charge, a stopped loop, a misfire, both channels in turn
  and a high side stuck on; flash waits out a fire;
- the test watches the plant every 25 us between loops, so a gate, a pump
  edge or U9 conducting is seen when it happens, not only at a loop.

## The HTTP server as a byte stream (WEB-HTTP-01..04)

`test_http.c` (target `http_tests`) drives `http_conn.c` through a fake
transport: each request whole, split at every byte position, byte by byte and
in random pieces, drained through windows of 1 to 9 bytes. It covers framing,
long header blocks, HEAD, refusals (400/405/411/413/414/431), a peer that
closes early, a streamed body five times the ring into a sink that refuses,
a streamed response, a response larger than tx, `Expect: 100-continue`, which
connections want a service pass, a unit that answers away from the service
call (DD-073), and every route that writes a file waiting for the cleanup
(WEB-API-21, DD-108). `support/http_stream_check.py <board-ip>` does the same
over raw sockets on a board.

## On hardware

`support/api_check.py <board-ip>` checks the HTTP API on a live board,
`support/http_stream_check.py` the server against awkward TCP,
`support/failed_write_check.py` an upload past the board's room and the
cleanup after it, `support/bench_flight.py` flies a profile through the
board's own flight software (with `--fail` for a canopy that never opens),
`support/pressure_trace.py` and `support/noise_baseline.py` judge the pressure
sensor, `support/test_network.py` the USB network, and
`test/web/hw_ui_check.js <board-ip>` runs the read-only UI checks. They need a
board, so CI does not run them. A requirement only they verify is marked
`✅ HW` in `TRACEABILITY.md`.

## In CI

`.github/workflows/build.yml` runs on every push to `main`, `lua-all-boards`
and `freertos-sd`, and on pull requests to `main`. It builds MK1B (`build`),
MK1C, MK1A, MK1C-SD, MK1B-8MB and the reference template, then runs
`scripts/run_host_tests.sh`, `prove_core0.py` on each board's ELF but the
template's, `prove_flight_ram.py --strict` on every ELF, the format and
complexity gates, the version rules, `pressure_trace.py --selftest`,
`scripts/sync_demo.sh --check`, the Playwright suite in all three modes and
the browser simulator, and the WASM build.

## Layout

```
test/
  hal_test.c             hal.h on mock state, with an in-memory filesystem
  mocks.h                the mock state: sensor model, pyro pulses, UART, time, stalls
  board_harness.c/.h     the board powered on and looped as main_hardware.c runs it
  flight_run.c/.h        a flight flown beside its truth
  collector_bus.h        the fake bus and clock under src/pressure_collector.c
  json_check.h           a strict JSON reader for the rendered routes
  estimator_replay.c     one estimator over a file of readings, for sim/study/lumped_port_check.py
  fake_sdk/, fake_rtos/  the Pico SDK and the scheduler under a board file or a task
  test_*.c               one file per suite (table above)
  web/                   Playwright UI tests, mock_server.js, static_server.js, hw_ui_check.js
test_data/
  open_rocket_export.csv OpenRocket simulation (228 points, 16 s flight, 165 ft peak)
```
