# Architecture — Autonomous I/O Around a Fixed-Period Loop

## Overview

Core0 runs one loop every 10 ms (`src/loop_period.h`, DD-104): the flight
task, alone on core0 under FreeRTOS SMP (DD-073). The sensors, telemetry TX
and the buzzer run on their own: the pressure collector's alarm interrupt,
the UART's transmit interrupt, and tasks the loop steps, with nothing
waiting (DD-053). The flight software steps once for every pressure sample
the loop finds (FLT-RATE-06) and emits events. The HAL handles buffering,
transport and storage. From the first period airborne to landing the loop
and core0's interrupts run from RAM, and core1 runs in each period's frame:
the log writer's 3 ms, whose flash operations stop nothing on core0, then
Lua's tick (DD-104). The net task -- USB, smallest_tcp and HTTP -- shares
core1 with them.

## Design Principles

1. Flight software reaches the hardware only through `hal.h` — no UART, GPIO, flash or filesystem calls of its own.
2. I/O runs without the loop waiting for it — both sensors' conversions are the collector's alarm handler's (DD-093), telemetry TX is an interrupt's, the buzzer is a task stepped once a loop.
3. The exec loop is the only clock — nothing sleeps or busy-waits; anything that waits parks on a deadline a later iteration checks (DD-053). The flight task's only blocking call is the wait for its next period.
4. Config changes are one-line operations — X-macro table generates struct, parser, serializer, defaults, range checks and the round-trip test.
5. Telemetry is event-driven — formatter is a separate module, HAL is raw transport.
6. A flash write never meets an XIP fetch: on the ground the other core is parked for it (DD-074); in flight core0 runs from RAM and the write runs on core1 in the writer's slot (DD-104).

## How It Works

| Aspect | How it works |
|---|---|
| Pressure | The collector (`src/pressure_collector.c`), one interrupt state machine in RAM, commands each conversion, waits out the part's worst case on an alarm, reads, and queues up to four cycles with the time of each conversion (DD-093). MS5607: a pressure and a temperature every 18.8 ms, 53 pressures a second. BMP280: 69 to 73 a second |
| Processing | The pressure task compensates each queued cycle and feeds the reading to the pressure layer's ring (`pp_feed_us()`); `dispatch_state()` steps once for every sample waiting, each taken with `pp_read()` |
| Telemetry | `src/telemetry.c` builds the queued event sentences and the `$PYRO` state sentence once a second (DD-088); `hal_telemetry_send()` copies each into a 512-byte ring drained by the UART TX interrupt |
| Buzzer | `buzzer.c` encodes a pattern; its task steps it from `hal_tasks_tick()` |
| Data log | `hal_log_sample()` into a 4 KB RAM ring; the storage task writes it and syncs the file in the writer's slots (DD-062, DD-104) |
| Config | `hal_config_load()` returns a `config_t` built from the X-macro table |
| USB | the net task on core1: `tud_task()` from `hal_platform_service()`, woken by a TinyUSB event or each millisecond |
| HTTP | the net task: the transport moves bytes between smallest_tcp and each connection's rings, and handlers are work units run between its passes (DD-073, DD-102) |
| Flash writes | on the ground under a lockout that parks the other core; in flight the writer's alone, in pieces inside its slot (`src/flash_op.h`, `src/flash_slot.h`) |
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

/* Ground test switch */
bool hal_ground_test_asserted(void);

/* Platform */
void hal_tasks_tick(uint32_t now_ms);
void hal_platform_init(void);
void hal_platform_service(void);
```

## Telemetry Architecture

```
Flight Software → telemetry_queue() for an event; telemetry_send() once a second
Formatter       → src/telemetry.c: the queued event sentences, oldest first, then the $PYRO state sentence
HAL Transport   → hal_telemetry_send(sentence) — 512-byte ring, UART TX interrupt
```

Events, queued when they occur and carried by the next message (TEL-11):
- `$PYRO_APO` — the peak and the flight time
- `$PYRO_FIRE` — the channel, the height and the flight time
- `$PYRO_LAND` — the peak and the flight time

Periodic, once a second in PAD_IDLE, in flight and in LANDED (TEL-03):
- `telemetry_send(&queue, &snapshot)`

The queue holds 12 events and counts one it cannot hold. The ring is first
come, first served: bytes that do not fit are dropped. The flight task's
sentences and the net task's diagnostic lines share it, under a hardware
spin lock held for one line's copy.

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
`buzzer_play_usb_ok()` and `buzzer_play_ground_test()`; `buzzer_stop()`
silences it. The buzzer's task walks the pattern from `hal_tasks_tick()`,
once a loop, so each step ends on the first loop at or after its deadline.
No main-loop code beyond the tick.

## Config System

X-macro table is the single source of truth (`src/config_fields.h`), one
row a key: its type, field, key, default and range:

```c
#define CONFIG_FIELDS(X) \
    X(STR,   id,                   "id",                   "PYRO001",       0, 0)      \
    X(STR,   name,                 "name",                 "MyRocket",      0, 0)      \
    X(MODE,  pyro1_mode,           "pyro1_mode",           PYRO_MODE_DELAY, 0, 0)      \
    X(U16,   pyro1_value,          "pyro1_value",          0,               0, 65535)  \
    X(MODE,  pyro2_mode,           "pyro2_mode",           PYRO_MODE_AGL,   0, 0)      \
    X(U16,   pyro2_value,          "pyro2_value",          300,             0, 65535)  \
    X(UNITS, units,                "units",                UNITS_M,         0, 0)      \
    X(U16,   pyro1_refire_speed,   "pyro1_refire_speed",   0,               0, 65535)  \
    X(U16,   pyro2_refire_speed,   "pyro2_refire_speed",   0,               0, 65535)  \
    X(U16,   emergency_fire_speed, "emergency_fire_speed", 0,               0, 65535)  \
    X(U16,   refire_interval,      "refire_interval",      0,               0, 65535)  \
    X(U16,   fire_gap,             "fire_gap",             0,               0, 65535)  \
    X(STR,   estimator,            "estimator",            "constacc",      0, 0)      \
    X(U8,    landing_timeout,      "landing_timeout",      60,              0, 255)    \
    X(BOOL,  lua_enabled,          "lua_enabled",          false,           0, 0)      \
    X(U32,   lua_baud,             "lua_baud",             9600,            300, 921600) \
    X(U16,   lua_pixels,           "lua_pixels",           0,               0, 65535)
```

Generates: struct definition, INI parser, INI serializer, defaults, a
build-time check that each range fits its field, round-trip test.
Adding a field = one line. Parser and tests update automatically.
The hardware map (`pins.ini`) and the beep personalities (`beep.ini`) are
files of their own.

## Main Loop

`src/main_hardware.c`, in outline:

```c
int main() {
    hal_platform_init();
    flight_init(&ctx);
    pin_store_load(NULL, 0);
    pin_store_claim_pads();                 /* one owner per pad (DD-020) */
    hal_pyro_claim_channels(pin_store_pyro_pads);
    board_buzzer_set_pin(...);
    hal_ground_test_configure(...);
    lua_app_init(&ctx.config);              /* boards with Lua */
    rtos_start(&ctx);                       /* the tasks; never returns */
}

void flight_task(void *arg) {               /* core0, alone at P */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* the period's alarm */
        /* airborne with the log running: flash_op_flight_begin(), all from RAM */
        core1_frame_begin(period_t0);       /* the writer's slot, then Lua's */
        watchdog_update();
        hal_tasks_tick(now);                /* the pressure task, the buzzer, the switch */
        flight_call_service();              /* changes other tasks handed over */
        ctx->current_state = dispatch_state(ctx, now);  /* a step per sample */
        flight_update_outputs(ctx, now);    /* telemetry, pyro update */
        update_status(ctx, now);
        lua_app_service(ctx, now);
        lua_app_dispatch();                 /* Lua's tick request */
    }
}
```

On core1, at the same priority (`src/rtos/rtos_tasks.h`):

```c
net_task:      hal_platform_service();      /* TinyUSB, smallest_tcp, mDNS */
               http_server_period();        /* once a period */
               http_server_work(...);       /* up to four units */
               /* wait for a TinyUSB event, or 1 ms */
storage_task:  core1_frame_wait(...);       /* the head of the period */
               hal_storage_service(now);    /* the flight log, queued file work, the cleanup */
               flight_storage_service(ctx, now);
               core1_frame_writer_done();   /* the rest of the frame is Lua's */
lua_task:      the script's tick, in its slot of the frame
```

## Autonomous I/O Map

| Function | Mechanism | Trigger | CPU involvement |
|---|---|---|---|
| Pressure conversions, either sensor | The collector's alarm handler, in RAM | Its own alarm, free of the loop | A short handler per alarm on core0 |
| Pressure compensation | Loop task | Every loop | The cycles the collector queued |
| UART telemetry TX | UART TX interrupt | Data in ring | Per FIFO refill |
| USB, smallest_tcp | The net task, core1 | A TinyUSB event, or each millisecond | Core1's time beside the writer and Lua |
| Buzzer pattern | Loop task | Step deadline | A GPIO write per step |
| Flash log writes | The storage task, in the writer's slot | Every 200 ms after the holdoff, a nearly full ring, or the 1 s sync | Under 3 ms a slot; an erase in suspended pieces |
| HTTP handlers | Work units in the net task | Between transport passes | Bounded per unit |
| Lua | The Lua task, core1 | Its slot of each period's frame | Up to 6.5 ms a period |
| Flight software | The flight task, core0 | Every period | A step per sample |

## RAM Budget

The larger fixed buffers, from the code:

| Component | Size |
|---|---|
| Lua arena (`pyro_lua.c`) | 32,768 bytes |
| Task stacks (`rtos_tasks.h`, `lua_core1.c`): flight, net, storage, Lua | 4,096, 8,192, 8,192 and 16,384 bytes |
| Flight log ring (`hal_common.c`) | 4,096 bytes |
| Host log ring (`log_live.h`) | 4,096 bytes |
| Pressure trace (`pressure_trace.h`, 256 records) | 6,144 bytes |
| Network frame buffers (`net_glue.c`) | 3,028 bytes |
| Held frames (`net_txq.h`) | 256 bytes |
| Telemetry TX ring | 512 bytes |
| Flight ring (64 samples) | 1,024 bytes |
| Pressure layer ring (`PP_RING_SIZE`) | 16 samples |

## Where Each Element Is

| Element | In the code |
|---|---|
| X-macro config system | `config_fields.h`, `config_set_defaults()`, `config_parse_ini()`, `config_serialize_ini()` |
| HAL config API | `hal_config_load()` / `hal_config_save()` |
| CPU sleep | none: `hal_sleep_until_event()` is empty on the hardware |
| Autonomous pressure sampling | the collector (DD-093) feeding the pressure layer's ring |
| Telemetry formatter module | `src/telemetry.c`, `$PYRO` NMEA (DD-088) |
| Buzzer pattern player | an `async_task_t` stepped by `hal_tasks_tick()` |
| Flight processing | `dispatch_state()` steps once for every sample waiting, each through `pp_read()` (FLT-RATE-06) |
| Fire-and-forget flight log | `hal_log_start()`, `hal_log_sample()`, `hal_log_stop()`; binary records, written in the writer's slots |
| Non-blocking UART TX | an interrupt-driven 512-byte ring |
| USB off the flight loop | the net task on core1 (DD-073) |
