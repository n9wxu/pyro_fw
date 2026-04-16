---
date: 2026-04-06T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 442a2f1ceb4af3876d74430a66fd9476d19a0a6f
branch: main
repository: pyro_fw
topic: "BMP280 I2C Detection — address 0x77"
tags: [research, codebase, bmp280, i2c, pressure-sensor, hal]
status: complete
last_updated: 2026-04-06
last_updated_by: Joseph Julicher
last_updated_note: "Added follow-up: hardware pull-up analysis — SDA GP6 internal-only, 400kHz hardcoded, no timeout on blocking calls"
---

# Research: BMP280 I2C Detection — address 0x77

**Date**: 2026-04-06  
**Researcher**: Joseph Julicher  
**Git Commit**: `442a2f1ceb4af3876d74430a66fd9476d19a0a6f`  
**Branch**: main  
**Repository**: pyro_fw

## Research Question

The BMP280 is not being detected. The correct I2C address is the default address 0x77.

## Summary

The BMP280 driver (`src/bmp280_driver.c`) defines both `0x76` and `0x77` and probes **both** during detection — iterating `0x76` first, then `0x77`. The driver, bus initialization, and abstraction layer are entirely in-tree (no third-party library); pico-sdk `hardware_i2c` is the only external dependency. The I2C bus is initialized once in `pressure_sensor_init()` on `i2c1` at 400 kHz with SDA on GPIO 6 and SCL on GPIO 7. A notable code-path detail: `i2c_deinit(i2c1)` is called at line 591 of `hal_hardware.c` inside `hal_platform_init()`, which executes after `hal_pressure_init()` has already run.

## Detailed Findings

### I2C Address Configuration

**`src/bmp280_driver.c`, lines 13–14:**
```c
#define BMP280_ADDR_SDO_LOW  0x76
#define BMP280_ADDR_SDO_HIGH 0x77
```

Neither address is permanently selected at compile time. A module-level static `bmp280_addr` (line 25) holds the runtime-resolved address.

**`src/bmp280_driver.c`, lines 65–88 — `bmp280_detect()`:**
The detection function iterates both addresses in a `for` loop starting at `0x76`:
```c
for (uint8_t addr = BMP280_ADDR_SDO_LOW; addr <= BMP280_ADDR_SDO_HIGH; addr++) {
```
For each address it:
1. Reads register `0xD0` (REG_ID) via `i2c_write_blocking` + `i2c_read_blocking`.
2. Compares the result to `0x58` (`BMP280_CHIP_ID`).
3. On match, stores the address in `bmp280_addr`, reads 24-byte calibration block from register `0x88`, writes `CTRL_MEAS = 0x2F` (normal mode, P×4, T×1 oversampling) and `CONFIG = 0x00` (no IIR filter).
4. Returns `true` immediately; if neither address responds with `0x58`, returns `false`.

Address `0x77` (SDO/CSB pin high — the hardware default) is the **second** address tried.

### I2C Bus Initialization

**`src/pressure_sensor.c`:**

| Parameter | Value |
|---|---|
| I2C peripheral | `i2c1` |
| Frequency | 400,000 Hz (400 kHz) |
| SCL pin | GPIO 7 (`I2C_SCL_PIN`) |
| SDA pin (BMP280 path) | GPIO 6 (`BMP280_SDA`) |
| SDA pin (MS5607 fallback) | GPIO 10 (`MS5607_SDA`) |

`pressure_sensor_init()` (line 28) sequence:
1. `i2c_init(i2c1, 400000)` — line 29
2. `configure_i2c_pins(BMP280_SDA)` — sets GPIO 6 and GPIO 7 to `GPIO_FUNC_I2C`, enables internal pull-ups on both (lines 17–22)
3. Calls `bmp280_detect()`. On success: records `PRESSURE_SENSOR_BMP280`, returns immediately.
4. On failure: `release_i2c_pin(BMP280_SDA)` resets GPIO 6 back to `SIO` input, then reconfigures with GPIO 10 as SDA and tries `ms5607_detect()`.

### `i2c_deinit` Call in hal_hardware.c

**`src/hal_hardware.c`, line 591:**
```c
i2c_deinit(i2c1);
```
This call appears near the end of `hal_platform_init()`. The startup ordering is:
- `hal_pressure_init()` (line 239) → calls `pressure_sensor_init()` → initializes bus + runs detection
- … other hardware init …
- `i2c_deinit(i2c1)` (line 591) — called after `hal_pressure_init()` has completed

### Async Read Path (50 Hz)

**`src/hal_hardware.c`, lines 221–223:**
Every 20 ms `pres_tick()` fires. When `sensor_type == 2` (BMP280), it calls `bmp280_read()` directly.

**`src/bmp280_driver.c`, lines 90–127 — `bmp280_read()`:**
- Burst-reads 6 bytes from register `0xF7` (PRESS_MSB) using `bmp280_read_reg()` with stored `bmp280_addr`.
- Assembles 20-bit raw ADC values for pressure and temperature.
- Applies the BMP280 datasheet compensation formulas using stored calibration trim values.
- Writes `temperature_c` and `pressure_pa` into the output `pressure_reading_t` struct.

### No Third-Party Library

**`CMakeLists.txt`, lines 102–105, 153:**
The sensor code is entirely in-tree:
- `src/pressure_sensor.c` — abstraction layer / I2C owner
- `src/bmp280_driver.c` — BMP280 driver
- `src/ms5607_driver.c` — MS5607 fallback driver

Only `hardware_i2c` from the pico-sdk is linked externally.

### Data Flow

```
pressure_sensor_init()              [pressure_sensor.c:28]
  i2c_init(i2c1, 400000)            [pressure_sensor.c:29]
  configure_i2c_pins(GPIO 6, 7)     [pressure_sensor.c:31]
  bmp280_detect()                   [bmp280_driver.c:65]
    probe 0x76 → read reg 0xD0 (chip ID)
    probe 0x77 → read reg 0xD0 (chip ID)
    on 0x58 match: read calib regs 0x88–0x9F (24 bytes)
                   write CTRL_MEAS=0x2F, CONFIG=0x00
                   store addr in bmp280_addr

hal_platform_init() later calls:
  i2c_deinit(i2c1)                  [hal_hardware.c:591]

Every 20ms — pres_tick()            [hal_hardware.c:170]
  bmp280_read()                     [bmp280_driver.c:90]
    i2c burst-read 6 bytes @ 0xF7 on i2c1, addr=bmp280_addr
    compensate → {pressure_pa, temperature_c}
  pres_append() → pp_feed()         [pressure_processing.c:136]
```

## Code References

- `src/bmp280_driver.c:13-14` — `BMP280_ADDR_SDO_LOW 0x76` / `BMP280_ADDR_SDO_HIGH 0x77`
- `src/bmp280_driver.c:25` — `static uint8_t bmp280_addr` (runtime-resolved)
- `src/bmp280_driver.c:65-88` — `bmp280_detect()`: probes 0x76 then 0x77
- `src/bmp280_driver.c:90-127` — `bmp280_read()`: 6-byte burst read from 0xF7
- `src/pressure_sensor.c:11-13` — I2C pin defines (SCL=GPIO7, BMP280 SDA=GPIO6)
- `src/pressure_sensor.c:28-40` — `pressure_sensor_init()`: I2C bus init + sensor detection sequence
- `src/hal_hardware.c:239` — `hal_pressure_init()` call site
- `src/hal_hardware.c:221-223` — BMP280 branch in async tick
- `src/hal_hardware.c:591` — `i2c_deinit(i2c1)` call

## Architecture Documentation

The pressure sensor stack has three layers:

1. **Driver layer** (`bmp280_driver.c`, `ms5607_driver.c`): sensor-specific register access, compensation math. Each driver defines both its own I2C addresses (0x76/0x77) and uses `I2C_PORT = i2c1`. Neither driver owns bus initialization.

2. **Abstraction layer** (`pressure_sensor.c`): owns `i2c_init`, pin configuration, and sensor auto-detection. Tries BMP280 on GPIO 6 first; falls back to MS5607 on GPIO 10. Returns a `pressure_sensor_type_t` enum to the HAL.

3. **HAL layer** (`hal_hardware.c`): registers a 50 Hz async tick, dispatches reads to the appropriate driver based on `sensor_type`, feeds compensated values into `pressure_processing.c`.

`pressure_processing.c` has no I2C or sensor knowledge — it operates on already-compensated Pa values.

## Follow-up Research — 2026-04-06: `/api/status` reports sensor = NONE

### How "None" reaches `/api/status`

`/api/status` is served by `serve_api_status()` in `src/http_server.c:186`, which calls `pressure_sensor_name()` directly. `pressure_sensor_name()` (`src/pressure_sensor.c:55`) switches on `detected_sensor`:

```c
const char *pressure_sensor_name(void) {
    switch (detected_sensor) {
        case PRESSURE_SENSOR_MS5607: return "MS5607";
        case PRESSURE_SENSOR_BMP280: return "BMP280";
        default: return "None";
    }
}
```

`detected_sensor` is a module-level static initialized to `PRESSURE_SENSOR_NONE` (line 15) and is only written in `pressure_sensor_init()`. When both `bmp280_detect()` and `ms5607_detect()` return false, `pressure_sensor_init()` returns `PRESSURE_SENSOR_NONE` (line 44) without writing `detected_sensor` — so it stays `PRESSURE_SENSOR_NONE` and `pressure_sensor_name()` returns `"None"`.

### Full Call Chain from Boot

```
main()                               [main_hardware.c:55]
  hal_platform_init()                [hal_hardware.c:557]
    board_init(), tud_init(), ...
    net_mac_init()                   [net_glue.c:53] ← no-op (hardcoded MAC)
    i2c_deinit(i2c1)                 [hal_hardware.c:591]  ← defensive cleanup;
                                       nothing in hal_platform_init() initialized i2c1
  flight_init(&ctx)                  [flight_states.c:615]
    hal_pressure_init()              [flight_states.c:625]
      pressure_sensor_init()         [hal_hardware.c:240]
        i2c_init(i2c1, 400000)       [pressure_sensor.c:29]
        configure_i2c_pins(GPIO 6)   [pressure_sensor.c:31]
        bmp280_detect()              [bmp280_driver.c:65]
          → probe 0x76: i2c_write_blocking → if fail, continue
          → probe 0x77: i2c_write_blocking → if fail, return false
        if both fail → return PRESSURE_SENSOR_NONE
        detected_sensor stays PRESSURE_SENSOR_NONE
      hw_sensor_type = 0             [hal_hardware.c:240]
      fifo NOT started               [hal_hardware.c:241-242]
```

### Why `i2c_deinit` Does Not Interfere

`hal_platform_init()` calls `i2c_deinit(i2c1)` at line 591 as a **defensive cleanup** — nothing in `hal_platform_init()` calls `i2c_init(i2c1, ...)`. This ensures i2c1 is in a clean reset state before `flight_init()` reinitializes it via `pressure_sensor_init()`.

### What `bmp280_detect()` Returns False Means

`bmp280_detect()` (`src/bmp280_driver.c:65-88`) returns false in three cases:
1. `i2c_write_blocking` returns `!= 1` for **both** 0x76 and 0x77 — device not ACKing on the bus
2. `i2c_read_blocking` fails to return the expected number of bytes
3. The chip ID register (`0xD0`) returns a value other than `0x58` for both addresses
4. The calibration burst-read (24 bytes from `0x88`) fails after a matching chip ID

The code path for address 0x77 is identical to 0x76 — there is no address-specific bias.

---

## Follow-up Research — 2026-04-06: Hardware pull-up context

### Confirmed hardware state (from developer)

| Signal | GPIO | External pull-up | Internal pull-up |
|---|---|---|---|
| SDA (BMP280) | GP6 | **None** | Yes (`gpio_pull_up`) |
| SCL | GP7 | 4.7 kΩ | Yes (`gpio_pull_up`) |
| SDO (address select) | — | Pulled **high** | — |

SDO pulled high → BMP280 address is **0x77** (confirmed).

### I2C clock is hardcoded at 400 kHz

`pressure_sensor.c:29`:
```c
i2c_init(i2c1, 400000);
```
There is no runtime-configurable clock parameter. The single literal `400000` is the only place the I2C bus speed is set. Both drivers (`bmp280_driver.c`, `ms5607_driver.c`) inherit this rate via the shared `i2c1` peripheral — neither driver calls `i2c_init`.

### All blocking calls use the indefinite-timeout variants

Every I2C transaction in both drivers uses `i2c_write_blocking` and `i2c_read_blocking` (no `_until` or `_timeout` suffix). These block forever if the bus is held low. There are no timeout guards anywhere in the sensor stack.

**All call sites:**
- `bmp280_driver.c:34` — `i2c_write_blocking` (write register + value)
- `bmp280_driver.c:38` — `i2c_write_blocking` (write register address, nostop=true)
- `bmp280_driver.c:40` — `i2c_read_blocking` (read data)
- `ms5607_driver.c:26,32,70` — `i2c_write_blocking`
- `ms5607_driver.c:34,73` — `i2c_read_blocking`

### RP2040 internal pull-up characteristics

The RP2040 datasheet specifies internal pull-up resistance of **50–80 kΩ**. The I2C fast-mode (400 kHz) specification requires SDA rise time ≤ 300 ns. The RC time constant formed by ~65 kΩ (nominal internal) and bus capacitance determines rise time; even ~5 pF of PCB trace capacitance gives τ ≈ 325 ns — at or beyond the fast-mode limit with zero margin.

SCL has a 4.7 kΩ external pull-up in parallel with the internal ~65 kΩ, giving an effective ~4.4 kΩ, well within spec. SDA has only the internal ~65 kΩ.

### Standard-mode (100 kHz) rise-time budget

At 100 kHz (standard mode), the maximum SDA rise time is **1000 ns** — more than 3× the fast-mode limit. The same ~65 kΩ / ~5 pF combination gives τ ≈ 325 ns, well within the 1000 ns budget with significant margin.

## Open Questions

- Whether `i2c_deinit(i2c1)` at `hal_hardware.c:591` is intentional cleanup or a latent bug (the call appears defensive since nothing in `hal_platform_init` initializes i2c1).
- What the actual PCB trace capacitance on GP6 is — determines whether 400 kHz is viable even with an added external pull-up.
- Whether `i2c_write_blocking` hangs at the 0x76 probe step (blocking forever before even reaching 0x77), or whether it NAKs cleanly and advances the loop.
