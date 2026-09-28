# Architecture v2 — Autonomous I/O Around a Fixed-Period Loop

## Overview

Core0 runs one loop every 20 ms (`src/loop_period.h`, DD-065). The sensors,
telemetry TX and the buzzer run on their own: interrupt handlers and
cooperative tasks the loop steps, with nothing waiting (DD-053). The flight
software reads one sample at a time from the pressure layer and emits events.
The HAL handles buffering, transport and storage. Every flash write runs in a
window the loop opens between core1's work units. The loop's slack serves USB,
lwIP and HTTP. On the boards with Lua, core1 runs Lua and the HTTP units
core0 hands it (DD-061).

## Design Principles

1. Flight software reaches the hardware only through `hal.h` — no UART, GPIO, flash or filesystem calls of its own.
2. I/O runs without the loop waiting for it — the MS5607's conversions are an alarm handler's, telemetry TX is an interrupt's, the buzzer and the BMP280 are tasks stepped once a loop.
3. The exec loop is the only clock — nothing sleeps or busy-waits; anything that waits parks on a deadline a later iteration checks (DD-053). The slack is spent on USB, lwIP and HTTP, not sleep.
4. Config changes are one-line operations — X-macro table generates struct, parser, serializer, defaults, and test.
5. Telemetry is event-driven — formatter is a separate module, HAL is raw transport.
6. Flash is written only in the window — core1 is idle and no XIP fetch can meet a program or erase (DD-035).

## How It Works

| Aspect | How it works |
|---|---|
| Pressure | MS5607: an alarm handler in RAM commands, stamps and reads a pressure and a temperature each loop (DD-051, DD-066). BMP280: one forced conversion a loop, taken at the next (DD-067). 50 pressures a second either way |
| Processing | Each reading goes straight into the pressure layer's ring; the detectors take it with `pp_read()`, one sample per dispatch |
| Telemetry | `telemetry_formatter.c` builds NMEA or JSON; `hal_telemetry_send()` queues it on a 512-byte ring drained by the UART TX interrupt |
| Buzzer | `buzzer.c` encodes a pattern; its task steps it from `hal_tasks_tick()` |
| Data log | `hal_log_sample()` into a 4 KB RAM buffer; written and synced in the flash window (DD-035, DD-062) |
| Config | `hal_config_load()` returns a `config_t` built from the X-macro table |
| USB | `tud_task()` in the loop's platform stage and throughout the slack |
| HTTP | the transport moves bytes at the loop's head; handlers are work units in the slack or on core1 (DD-061) |
| Flash writes | only inside the window core0 opens once core1 is idle (`src/flash_window.h`) |
| CPU sleep | none: `hal_sleep_until_event()` is empty on the hardware |

## HAL Interface

A subset of `src/hal.h`:

```c
/* Time */
uint32_t hal_time_ms(void);

/* Pressure: brought up a step a loop; samples reach the pressure layer from the task */
void hal_pressure_init(void);
int hal_pressure_sensor(void);       /* -1 while bringing up */

/* Pyro */
void hal_pyro_init(void);
void hal_pyro_sample(void);
void hal_pyro_get(uint8_t channel, hal_continuity_t *out);
void hal_pyro_fire(uint8_t channel);
void hal_pyro_update(uint32_t now_ms);
bool hal_pyro_is_firing(void);
bool hal_pyro_fault(uint8_t channel);

/* Buzzer: raw on/off; buzzer.c's task sequences it */
void hal_buzzer_init(void);
void hal_buzzer_tone_on(void);
void hal_buzzer_tone_off(void);
void hal_buzzer_task_register(struct async_task *task);

/* Telemetry: raw async transport, best-effort */
void hal_telemetry_send(const char *sentence);

/* Data log: fire and forget */
void hal_log_start(const config_t *cfg, int32_t ground_pressure_pa);
void hal_log_sample(uint32_t time_ms, int32_t pressure_pa, int32_t altitude_cm,
                    uint8_t state, uint8_t under_thrust, uint8_t event);
void hal_log_stop(void);

/* Config: abstract storage */
int hal_config_load(config_t *cfg);
int hal_config_save(const config_t *cfg);

/* Ground test */
bool hal_serial_readline(char *buf, int max_len);
bool hal_ground_test_asserted(void);

/* Platform */
void hal_tasks_tick(uint32_t now_ms);
void hal_platform_init(void);
void hal_platform_service(void);
```

## Telemetry Architecture

```
Flight Software → telemetry_state(), telemetry_apogee(), telemetry_pyro_fire(), telemetry_landing()
Formatter       → builds protocol-specific message (NMEA or JSON)
HAL Transport   → hal_telemetry_send(sentence) — 512-byte ring, UART TX interrupt
```

Event functions (called once when event occurs):
- `telemetry_apogee(max_alt_cm, flight_time_ms)`
- `telemetry_pyro_fire(channel, alt_cm, time_ms)`
- `telemetry_landing(max_alt_cm, flight_time_ms)`

Periodic function, at `telem_rate_hz` in flight and 1 Hz on the ground:
- `telemetry_state(&snapshot)`

The formatter reads `config.telem_format` to select the protocol. The ring is
first come, first served: a sentence that does not fit loses its tail.

## Buzzer Architecture

`buzzer.c` encodes a request into a pattern (sequence of on/off durations):

```c
buzzer_pattern_t pattern[] = {
    {30, true}, {30, false},   /* chirp */
    {100, true}, {200, false}, /* beep */
    {0, false}                 /* end marker */
};
```

Requests are `buzzer_play_spec()` (a beep outcome), `buzzer_play_altitude()`,
`buzzer_play_usb_ok()` and `buzzer_play_ground_test()`. The buzzer's task
walks the pattern from `hal_tasks_tick()`, once a loop, so each step ends on
the first loop at or after its deadline. No main-loop code beyond the tick.

## Config System

X-macro table is the single source of truth (`src/config_fields.h`):

```c
#define CONFIG_FIELDS(X) \
    X(STR,     id,              "id",              "PYRO001") \
    X(STR,     name,            "name",            "MyRocket") \
    X(MODE,    pyro1_mode,      "pyro1_mode",      PYRO_MODE_DELAY) \
    X(U16,     pyro1_value,     "pyro1_value",     0) \
    X(MODE,    pyro2_mode,      "pyro2_mode",      PYRO_MODE_AGL) \
    X(U16,     pyro2_value,     "pyro2_value",     300) \
    X(UNITS,   units,           "units",           1) \
    X(U8,      telem_format,    "telem_format",    0) \
    X(U8,      telem_rate_hz,   "telem_rate_hz",   10) \
    X(LOGRATE, log_rate,        "log_rate",        LOG_RATE_1HZ) \
    X(U8,      landing_timeout, "landing_timeout", 60) \
    X(BOOL,    lua_enabled,     "lua_enabled",     false) \
    X(U16,     lua_baud,        "lua_baud",        9600) \
    X(U16,     lua_pixels,      "lua_pixels",      0)
```

Generates: struct definition, INI parser, INI serializer, defaults, round-trip test.
Adding a field = one line. Parser and tests update automatically.
The hardware map (`pins.ini`) and the beep personalities (`beep.ini`) are
files of their own.

## Main Loop

`src/main_hardware.c`, in outline:

```c
hal_platform_init();
flight_init(&ctx);
pin_store_load(NULL, 0);
pin_store_claim_pads();                 /* one owner per pad (DD-020) */
hal_pyro_claim_channels(pin_store_pyro_pads);
lua_app_init(&ctx.config);              /* launches core1 */

while (1) {                             /* every LOOP_PERIOD_MS */
    hal_tasks_tick(now);                /* sensor first, then the buzzer and the switch */
    hal_platform_service();             /* TinyUSB, lwIP, HTTP transport, mDNS */
    ctx.current_state = dispatch_state(&ctx, now);
    flight_update_outputs(&ctx, now);   /* telemetry, pyro update */
    lua_app_service(&ctx, now);
    if (window) {                       /* core1 idle, no MK1C fire in progress */
        hal_flash_service(now);         /* the flight log's writes and syncs */
        flight_flash_service(&ctx, now);
    }
    http_server_period();
    lua_app_dispatch(...);              /* core1's grant: HTTP units, then Lua */
    while (before the deadline) {       /* the slack */
        tud_task();
        net_service();
        http_server_work(remaining);    /* one unit, with its budget left */
    }
}
```

## Autonomous I/O Map

| Function | Mechanism | Trigger | CPU involvement |
|---|---|---|---|
| MS5607 conversions | Alarm handler in RAM | Loop's pressure task | ~0.2 ms per conversion on core0 |
| BMP280 conversions | Loop task, forced mode | Every loop | One burst read and a command |
| UART telemetry TX | UART TX interrupt | Data in ring | Per FIFO refill |
| USB (tud_task) | Loop | Platform stage and the slack | Slack time |
| Buzzer pattern | Loop task | Step deadline | A GPIO write per step |
| Flash log writes | Loop, flash window | 200 ms after the holdoff, a full buffer, or the 1 s sync | Tens of ms per sector erase |
| HTTP handlers | Work units | Slack, or core1's grant | Bounded per unit |
| Flight software | Loop | Every period | Per sample |

## RAM Budget

The larger fixed buffers, from the code:

| Component | Size |
|---|---|
| Flight log buffer (`hal_common.c`) | 4,096 bytes |
| Log plan delay line (`log_plan.h`, 128 rows) | ~3 KB |
| Pressure trace (`pressure_trace.h`, 256 records) | 6,144 bytes |
| lwIP heap (`MEM_SIZE`) | 8,000 bytes |
| Telemetry TX ring | 512 bytes |
| Flight ring (64 samples) | 1,024 bytes |
| Pressure layer ring (`PP_RING_SIZE`) | 64 samples |

## Implementation Tasks

| Task | What | State |
|---|---|---|
| 1 | X-macro config system | Built: `config_fields.h`, `config_set_defaults()`, `config_parse_ini()`, `config_serialize_ini()` |
| 2a | HAL config API | Built: `hal_config_load()` / `hal_config_save()` |
| 2b | Ground test serial commands | Built: `src/ground_test.c`, `$GT` responses, 3 s auto-disarm, PAD_IDLE only. The switch procedure (DD-071) sits beside it |
| 3 | CPU sleep | Not used: `hal_sleep_until_event()` is empty on the hardware; the slack serves USB and HTTP |
| 4 | Autonomous pressure sampling | Built differently: per-loop conversions (DD-051, DD-066, DD-067) feed the pressure layer's ring. The 5-sample batch FIFO in `hal_common.c` is filled but not read |
| 5 | Telemetry formatter module | Built: `src/telemetry_formatter.c`, NMEA and JSON |
| 6+7 | Buzzer pattern player | Built: an `async_task_t` stepped by `hal_tasks_tick()` |
| 8 | Batch flight processing | Replaced: `dispatch_state()` takes one sample per call through `pp_read()` |
| 9 | Fire-and-forget flight log | Built: `hal_log_start()`, `hal_log_sample()`, `hal_log_stop()`; binary records, written in the flash window |
| 10 | Non-blocking UART TX | Built as an interrupt-driven 512-byte ring, not DMA |
| — | USB on core1 or a timer | Not built: USB runs in the loop; core1 runs Lua and HTTP units |
