# Architecture v2.0 — Autonomous I/O with Sleep-Optimized Flight Software

## Overview

Refactor the flight computer from synchronous pull-based I/O to autonomous
hardware-driven I/O with the CPU sleeping between 100ms processing windows.
All I/O runs via ISR/DMA/Core1. The flight software processes batches of
samples and emits events. The HAL handles all buffering, transport, and storage.

## Design Principles

1. Flight software has no I/O knowledge — no files, no UART, no GPIO, no flash.
2. All I/O is autonomous — pressure sampling, telemetry TX, buzzer patterns, USB servicing run without CPU.
3. CPU sleeps between events — wakes every 100ms to process a 5-sample buffer.
4. Config changes are one-line operations — X-macro table generates struct, parser, serializer, defaults, and test.
5. Telemetry is event-driven — formatter is a separate module, HAL is raw transport.

## Current vs Proposed

| Aspect | Current (v1.5) | Proposed (v2.0) |
|---|---|---|
| Pressure | Synchronous `hal_pressure_read()` | Autonomous 50Hz into ping-pong buffers |
| Processing | One sample per state tick | 5-sample batch every 100ms |
| Telemetry | Flight software formats NMEA | Separate formatter module |
| Buzzer | `buzzer_update()` called from main loop | Pattern buffer played by timer ISR |
| Data log | Ring buffer + batch CSV | `hal_log_sample()` fire-and-forget |
| Config | Flight software parses INI | `hal_config_load()` returns struct |
| USB | `tud_task()` in main loop | Timer ISR or Core1 |
| Flash writes | XIP stall during main loop | HAL buffers, writes when CPU sleeps |
| RAM | 66KB (64KB ring buffer) | ~1.2KB |
| CPU sleep | 0% | ~97-99% |

## HAL Interface

```c
/* Time */
uint32_t hal_time_ms(void);
void hal_sleep_until_event(void);

/* Pressure: autonomous 50Hz, dual-buffer, 5 samples per batch */
void hal_pressure_start(void);
hal_sample_buffer_t *hal_pressure_get_buffer(void);
void hal_pressure_release_buffer(void);

/* Pyro */
void hal_pyro_init(void);
void hal_pyro_check(hal_continuity_t *p1, hal_continuity_t *p2);
void hal_pyro_fire(uint8_t channel);
bool hal_pyro_is_firing(void);
bool hal_pyro_fault(uint8_t channel);

/* Buzzer: pattern-driven, played by timer ISR */
void hal_buzzer_play(const buzzer_pattern_t *pattern);
void hal_buzzer_stop(void);
bool hal_buzzer_is_active(void);

/* Telemetry: raw async transport, best-effort, events prioritized */
void hal_telemetry_send(const char *data, int len);

/* Data log: fire and forget */
void hal_log_header(const config_t *cfg, int32_t ground_pressure);
void hal_log_sample(uint32_t time_ms, int32_t pressure, int32_t altitude,
                    uint8_t state, uint8_t thrust, uint8_t event);

/* Config: abstract storage */
int hal_config_load(config_t *cfg);
int hal_config_save(const config_t *cfg);

/* Platform */
void hal_platform_init(void);
```

## Telemetry Architecture

```
Flight Software → telemetry_state(), telemetry_apogee(), telemetry_pyro_fire()
Formatter       → builds protocol-specific message (NMEA, binary, Eggtimer)
HAL Transport   → hal_telemetry_send(bytes, len) — async queue, ISR/DMA TX
```

Event functions (called once when event occurs):
- `telemetry_apogee(max_alt_cm, flight_time_ms)`
- `telemetry_pyro_fire(channel, alt_cm, time_ms)`
- `telemetry_landing(max_alt_cm, flight_time_ms)`
- `telemetry_continuity(p1_adc, p2_adc, flags)`

Periodic function (called every processing cycle):
- `telemetry_state(state, alt_cm, press_pa)`

The formatter reads `config.telem_format` to select the protocol.
The HAL prioritizes event messages over state updates internally.

## Buzzer Architecture

Flight software generates patterns (sequence of on/off durations):

```c
buzzer_pattern_t pattern[] = {
    {30, true}, {30, false},   /* chirp */
    {500, false},              /* pause */
    {100, true}, {200, false}, /* beep code */
    {0, false}                 /* end marker */
};
hal_buzzer_play(pattern);
```

Timer ISR walks the pattern autonomously. No CPU involvement after loading.

## Config System

X-macro table is the single source of truth:

```c
#define CONFIG_FIELDS(X) \
    X(STR,  id,            "id",            "PYRO001") \
    X(STR,  name,          "name",          "MyRocket") \
    X(U8,   units,         "units",         1) \
    X(MODE, pyro1_mode,    "pyro1_mode",    PYRO_MODE_DELAY) \
    X(U16,  pyro1_value,   "pyro1_value",   0) \
    X(MODE, pyro2_mode,    "pyro2_mode",    PYRO_MODE_AGL) \
    X(U16,  pyro2_value,   "pyro2_value",   300) \
    X(U8,   telem_format,  "telem_format",  0) \
    X(U8,   telem_rate_hz, "telem_rate_hz", 10) \
    X(U8,   log_rate_hz,   "log_rate_hz",   50) \
    X(BOOL, log_enabled,   "log_enabled",   true) \
    X(U8,   beep_mode,     "beep_mode",     0) \
    X(BOOL, buzzer_startup,"buzzer_startup", true)
```

Generates: struct definition, INI parser, INI serializer, defaults, round-trip test.
Adding a field = one line. Parser and tests update automatically.

## Main Loop

```c
hal_platform_init();
config_t cfg;
hal_config_load(&cfg);
flight_init(&ctx, &cfg);
hal_pressure_start();

while (1) {
    hal_sample_buffer_t *buf = hal_pressure_get_buffer();
    if (buf) {
        flight_process_samples(&ctx, buf->samples, buf->timestamps, buf->count);
        hal_pressure_release_buffer();
    }
    hal_sleep_until_event();
}
```

## Autonomous I/O Map

| Function | Mechanism | Trigger | CPU involvement |
|---|---|---|---|
| Pressure sampling | ISR/DMA/Core1 | 50Hz timer | None |
| UART telemetry TX | DMA | Data in queue | None |
| USB (tud_task) | Timer ISR / Core1 | 1ms timer | None |
| Buzzer pattern | Timer ISR | Pattern loaded | None |
| Flash CSV writes | Main loop | hal_log_sample buffer full | ~2ms per flush |
| Flight software | Main loop | Buffer ready (100ms) | ~1ms per batch |

## RAM Budget

| Component | Current | Proposed |
|---|---|---|
| Ring buffer | 65,536 bytes | 0 |
| Pressure ping-pong | 0 | 120 bytes |
| Telemetry TX queue | 0 | 256 bytes |
| CSV write buffer (HAL) | 0 | 512 bytes |
| Buzzer pattern | ~50 bytes | ~64 bytes |
| Flight context | ~200 bytes | ~200 bytes |
| **Total** | **~66KB** | **~1.2KB** |

## Implementation Tasks

### Task 1: X-macro config system
- Create `config_fields.h` with X-macro table
- Generate: `config_t` struct, `config_set_defaults()`, `config_parse()`, `config_serialize()`
- Round-trip test: serialize → parse → compare every field
- Move INI parser out of flight_states.c into config module

### Task 2: Refactor HAL interface
- Update `hal.h` with new API
- Remove: `hal_pressure_read()`, `hal_fs_*`, `hal_buzzer_tone_on/off()`
- Add: `hal_pressure_get_buffer()`, `hal_log_sample()`, `hal_config_load/save()`, `hal_buzzer_play()`, `hal_sleep_until_event()`

### Task 3: Telemetry formatter module
- Create `telemetry.h` with event functions
- Formatter reads `config.telem_format` to select protocol
- Calls `hal_telemetry_send(bytes, len)`

### Task 4: Buzzer pattern generator
- Refactor `buzzer.c` to generate pattern arrays
- `buzzer_make_startup(code)` → pattern
- `buzzer_make_altitude(altitude)` → pattern

### Task 5: Refactor flight_states.c for batch processing
- `flight_process_samples(ctx, samples, timestamps, count)`
- Remove ring buffer from context
- Events call telemetry formatter and `hal_log_sample()` directly

### Task 6: Test HAL (hal_test.c)
- Pressure: fill buffer from mock data
- Log: capture samples in memory
- Config: in-memory storage
- Buzzer: capture pattern

### Task 7: Hardware HAL (hal_hardware.c)
- Pressure: timer ISR into ping-pong buffers
- Telemetry: DMA UART TX
- Buzzer: timer ISR pattern player
- Log: RAM buffer, flush to littlefs
- Config: littlefs INI via X-macro
- USB: 1ms timer ISR
- Sleep: WFE

### Task 8: Simulation HAL (hal_sim.c)
- Pressure: physics engine fills buffers
- Telemetry/Log/Config: in-memory
- Buzzer: export pattern for Web Audio

### Task 9: Update tests
- Batch processing API in integration/closed-loop tests
- Config round-trip test
- Telemetry formatter tests
- Buzzer pattern tests

### Task 10: Update documentation
- REQUIREMENTS.md: autonomous I/O, sleep, config system requirements
- TRACEABILITY.md: update test mappings
- IMPLEMENTATION.md: new architecture
- README.md: battery-optimized, autonomous I/O features
