# Board support packages

Each directory here is a self-contained board. The top-level `CMakeLists.txt`
names no board and never needs editing to add one.

## Porting to new hardware

```bash
cp -r boards/reference boards/mk1d          # 1. copy
$EDITOR boards/mk1d/board_pins.h            # 2. implement (every TODO)
$EDITOR boards/mk1d/board.cmake             # 3. fix up cmake
cmake -B build-mk1d -DPYRO_BOARD=mk1d       # 4. build
cmake --build build-mk1d
```

An unknown board name fails the configure and lists what is available.

## Board kinds

A board declares a `PYRO_BOARD_KIND` in its `board.cmake`. The top level
branches on the kind, never on a board name, so adding a board of an existing
kind still requires no edit to shared CMake.

| Kind | Toolchain | HAL | Output |
|---|---|---|---|
| `pico` | Pico SDK, arm-none-eabi | `src/hal_common` + `src/board_if.h` | `.uf2` behind `pico_fota_bootloader` |
| `host` | host cc, or `emcc` for WASM | its own complete `hal.h` | native binary / `.wasm` |

A `host` board supplies `PYRO_HOST_HAL_SOURCES` instead of a `pyro_board`
library, because the host and WASM builds are custom targets rather than SDK
targets. `boards/sim` is the worked example.

## What a board must provide

| File | Implements | Notes |
|---|---|---|
| `board_pins.h` | identity, capabilities, pin map, bus speeds | `BOARD_NAME_STR`, `BOARD_SHORT_STR`, `BOARD_HAS_*`, `BOARD_MS5607_I2C_HZ` / `BOARD_BMP280_I2C_HZ` |
| `pin_caps.h` | what each pin MAY become | `BOARD_PIN_CAPS`, topology, protection class, `LUA_PIN_LIST` |
| `hal_board.c` | `src/board_if.h` | 9 functions: lifecycle, LED, buzzer, UART |
| `pyro_board.c` | `src/pyro.h` | 6 functions |
| `pressure_board.c` | `src/pressure_sensor.h` | 3 functions |
| `board.cmake` | pre-SDK settings | `PICO_BOARD`, flash geometry |
| `CMakeLists.txt` | sources and target | must export `pyro_board` |
| `sdk/<name>.h` | Pico SDK board header | only for a non-Pico-module board |

Everything else — the UART ISR ring buffer, littlefs, config persistence, the
async task runner, flight logging, USB and networking — lives in
`src/hal_common/` and is shared. A board never copies it.

### Sensor bus speeds

Each board sets its own I2C speed per sensor (DD-052): the device's fastest
(`MS5607_I2C_MAX_HZ`, `BMP280_I2C_MAX_HZ`), or slower where the PCB cannot
carry it. Fast mode's 300 ns rise needs pull-ups of at most
300 ns / (0.8473 x Cb), 4k7 to about 75 pF (docs/datasheets/, UM10204 pages 44
and 50); the RP2040's own 50-80k pull-ups are too weak for it. Read the
pull-ups from the board's design files, not from memory. `pressure_board.c`
fails the build if a speed exceeds its device's.

### pin_caps.h

`board_pins.h` says where a function *is*. `pin_caps.h` says what each pin
*may become*, which is what lets configuration retask one without the pin map
leaving the board package — the split that keeps DD-012 substantially intact.

One row per assignable pin: the functions the hardware supports, and the pin's
place in the pyro power group. A pyro pad lists `FN_PYRO_*` **together with**
what it may become once its channel is released; holding both at once is an
assignment-time rule, not a property of the hardware. A pin with no row is not
assignable to anything, which is the right state for anything the flight
software must keep to itself.

`src/pin_model.h` holds the vocabulary and the `_Static_assert`s. A wrong row
fails the build: the sensor's I2C pads can never be Lua-assignable, an analog
function must be on an RP2040 ADC pad, a power-group role needs a pyro element
behind it, and each of `PG_CH1`, `PG_CH2` and `PG_COMMON` may appear once.

Copy `boards/reference/pin_caps.h` and replace every TODO.

`src/hal.h` and `src/pyro.h` are frozen contracts. If a board seems to need a
new HAL function, the behaviour probably belongs in the flight layer, which
already has the flight state.

## Why two CMake files

The Pico SDK consumes `PICO_BOARD` and `PICO_BOARD_HEADER_DIRS` during
`pico_sdk_init()`, and `pico_fota_bootloader` consumes the flash geometry when
it generates its linker script at FetchContent time. Both happen *before*
`add_subdirectory()` could run. So:

| | When | Purpose |
|---|---|---|
| `board.cmake` | `include()`d before `pico_sdk_init()` | `PICO_BOARD`, header dirs, flash geometry |
| `CMakeLists.txt` | `add_subdirectory()`d after | sources, target `pyro_board` |

## Two traps worth knowing

**`PICO_DEFAULT_LED_PIN` is not inert.** TinyUSB's BSP `board_init()` does
`gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT)` on whatever that macro
names. The stock `boards/pico.h` sets it to 25 — which on MK1C is `BIAS_B`, a
pyro bias injector. A custom SDK board header should leave it undefined and
drive the LED from `hal_board.c` instead.

**A custom SDK board header needs the CMake directive as well as the
`#define`.** `cmake/generic_board.cmake` greps board headers for
`pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, ...)` to set the
CMake-side variable, and `pico_fota_bootloader` substitutes
`@PICO_FLASH_SIZE_BYTES@` from it. Omit the directive and you get
`__FLASH_SIZE =  - __FILESYSTEM_SIZE` — an empty substitution producing a
negative flash size and a garbage A/B slot map **that still links without
error**. The top level has an explicit geometry guard because the bootloader's
own linker assertions do not catch it.

## Existing boards

| Board | Hardware | Pyro architecture |
|---|---|---|
| `mk1b` | Pico module, 2 MB flash | AP2192 high-side switches, common enable |
| `mk1c` | Bare RP2040, 16 MB flash | TPS259570 eFuse, software charge-pump arm |
| `reference` | template (`pico`) | safe stubs: no continuity, refuses to fire |
| `sim` | none (`host`) | simulated; runs natively or as WASM |

`reference` is not buildable hardware. It compiles, so a copy of it builds
before you have written anything, which is the point.

```bash
cmake -B build-sim -DPYRO_BOARD=sim
cmake --build build-sim --target sim     # native simulator
./scripts/build_wasm.sh                  # WASM, via Emscripten
```
