---
date: 2026-04-04T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "Why the LED on GPIO 25 is not lighting up or blinking"
tags: [research, codebase, led, gpio25, hal_hardware, pressure, uncommitted]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
---

# Research: Why the LED on GPIO 25 Is Not Lighting Up or Blinking

**Date**: 2026-04-04T00:00:00-07:00
**Researcher**: Joseph Julicher
**Git Commit**: `111637357a337d93f1c1f3f3953d3e908eff2717`
**Branch**: main
**Repository**: pyro_fw

## Research Question

Determine why the LED connected to GPIO 25 on RP2040 custom hardware is not lighting up or blinking.

## Summary

There are two layers to this answer:

**Layer 1 — The LED code exists only as uncommitted working-tree changes.**
Both the GPIO 25 initialization block and the blink toggle were added to `src/hal_hardware.c`
after the last commit (`1116373 v2-13`). They are visible in `git diff HEAD` as pure additions (`+`
lines only). Any firmware currently flashed to the device was built from the committed source,
which contains **no LED code whatsoever**. GPIO 25 is never claimed, never set as an output,
and is left in whatever state `board_init()` leaves it — meaning it cannot drive a LED.

**Layer 2 — Even after rebuilding and reflashing, the blink requires a live pressure sensor.**
The LED toggle is inside `pres_append()`, which is called only from the pressure async state
machine. That machine is started by `hal_pressure_init()` (called from `flight_init()` in
`src/flight_states.c:593`). If no pressure sensor is detected, the FIFO never starts,
`pres_append()` is never called, and the LED stays statically ON at boot without ever blinking.

---

## Detailed Findings

### 1. The LED Code Is Not in the Committed Build

`git diff HEAD -- src/hal_hardware.c` shows two pure addition blocks:

**Block A — init in `hal_platform_init()` (around line 593):**
```c
/* Proof-of-life LED — GPIO 25 (onboard LED on Pico).
 * Must be after board_init() which reinitializes this pin.
 * Start ON so a lit LED confirms correct GPIO at boot. */
gpio_init(25);
gpio_set_dir(25, GPIO_OUT);
gpio_put(25, 1);
```

**Block B — blink in `pres_append()` (around line 153):**
```c
static uint8_t led_n = 0;
if (++led_n >= HAL_PRESSURE_BATCH_SIZE) {
    led_n = 0;
    gpio_xor_mask(1u << 25);
}
```

Neither block exists in the HEAD commit. Both must be compiled and flashed before the LED
will respond at all.

### 2. `board_init()` Reclaims GPIO 25

`hal_platform_init()` calls `board_init()` (TinyUSB BSP) at `src/hal_hardware.c:588`.
The `board_init()` implementation for the Pico BSP reinitializes GPIO 25 as part of its own
setup. The comment in the working-copy code acknowledges this explicitly:

> "Must be after board_init() which reinitializes this pin."

In the committed code there is no follow-up `gpio_init(25)` call, so `board_init()` is the
last thing touching pin 25 and leaves it in a non-driven state. The LED stays off.

### 3. The Init and Blink Code Paths (Working Copy)

Once the working-copy changes are built and flashed, the LED follows this sequence:

| Stage | Code location | LED state |
|-------|--------------|-----------|
| `hal_platform_init()` runs | `src/hal_hardware.c:593–595` | Goes **ON** (gpio_put high) |
| Each `pres_append()` call (every 5th) | `src/hal_hardware.c:153–157` | **Toggles** at 5 Hz |
| Pressure sensor absent (hw_sensor_type == 0) | `src/hal_hardware.c:262–263` | Stays **ON** statically, never blinks |

### 4. Blink Is Gated on the Pressure Pipeline

The call chain for blink to work:

```
main()                              src/main_hardware.c:55
  └─ flight_init(&ctx)              src/main_hardware.c:59
       └─ hal_pressure_init()       src/flight_states.c:593
            └─ pressure_sensor_init()
            └─ hal_pressure_fifo_start(50)   [only if sensor found]
                 └─ registers pres_tick async task
                      └─ pres_tick() calls pres_append() on each completed sample
                           └─ gpio_xor_mask(1u << 25)  [every 5th sample]
```

`hal_pressure_init()` is defined at `src/hal_hardware.c:240`. It returns `hw_sensor_type`
(0 = not found, 1 = MS5607, 2 = BMP280). If `hw_sensor_type == 0`, `hal_pressure_fifo_start()`
is never called (`src/hal_hardware.c:242–243`), the `pres_tick` task is never registered, and
`pres_append()` is never called.

### 5. No `#ifdef` Guards Around Any LED Code

There are no conditional compilation guards anywhere in `hal_hardware.c`. All LED code
(once added) compiles unconditionally in every hardware build. The HAL target selection
(hardware vs. test vs. sim) is done at the CMake level, not via preprocessor switches within
the files.

### 6. `PICO_DEFAULT_LED_PIN` Is Not Defined in This Project

The project's `CMakeLists.txt` sets `PICO_BOARD pico` but does not override
`PICO_DEFAULT_LED_PIN`. The LED is referenced throughout `hal_hardware.c` as the literal
integer `25`, not via a named constant or SDK macro.

---

## Code References

- `src/hal_hardware.c:582–617` — `hal_platform_init()`: hardware init sequence
- `src/hal_hardware.c:588` — `board_init()` call (reclaims GPIO 25)
- `src/hal_hardware.c:593–595` — GPIO 25 init, dir set, driven HIGH *(working copy only)*
- `src/hal_hardware.c:125–158` — `pres_append()`: pressure sample handler with LED toggle
- `src/hal_hardware.c:153–157` — LED blink toggle via `gpio_xor_mask(1u << 25)` *(working copy only)*
- `src/hal_hardware.c:240–245` — `hal_pressure_init()`: sensor detection and FIFO start
- `src/hal_hardware.c:261–276` — `hal_pressure_fifo_start()`: registers async `pres_tick` task
- `src/flight_states.c:583–598` — `flight_init()`: calls `hal_pressure_init()` (line 593)
- `src/main_hardware.c:55–83` — `main()`: calls `hal_platform_init()` then `flight_init()`
- `CMakeLists.txt:27` — `set(PICO_BOARD pico ...)`: board selection, no LED pin override

---

## Architecture Documentation

GPIO 25 is controlled entirely within `hal_hardware.c`. There is no dedicated LED module,
no named constant for the pin, and no LED task in the async scheduler. The blink is a
side-effect of the pressure sampling pipeline: every 5 pressure samples (at 50 Hz = every
100 ms), `pres_append()` increments a static counter and fires `gpio_xor_mask`. The resulting
visible blink rate is 5 Hz.

The async task runner in `hal_hardware.c` holds up to 8 tasks (`HW_MAX_TASKS`). At runtime
the registered tasks are: `pres_tick` (pressure), `log_task_tick` (flash log), and the
buzzer task (registered via `hal_buzzer_task_register()`). The LED blink is not a task — it
runs as an inline side-effect of `pres_tick → pres_append`.

`hal_sleep_until_event()` is a no-op (disabled in v2.1.27, `src/hal_hardware.c:576–578`),
so the main loop busy-polls and `hal_tasks_tick()` runs continuously.

---

## Related Research

- [2026-04-04-led-not-toggling-5hz.md](2026-04-04-led-not-toggling-5hz.md) — Deeper analysis of why the toggle fired only once (the earlier `front_ready`-gated placement)

---

## Open Questions

- Does the I2C bus on the custom hardware connect a pressure sensor on the expected pins
  (GPIO 6/7 for BMP280 or GPIO 7/10 for MS5607)? If not, `hal_pressure_init()` returns 0
  and only the static-ON behavior is visible after reflashing.
- Has the firmware been rebuilt and reflashed since the working-copy changes were made?
  If not, the LED cannot light up at all regardless of hardware state.
