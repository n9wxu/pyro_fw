# Hardware Porting Guide

This document defines the hardware requirements for porting the Pyro MK1B
flight software to a new MCU, and analyzes several candidate platforms.

## System Hardware Requirements

These requirements are derived from the flight software, HAL interface,
and v2.0 autonomous I/O architecture (see ARCHITECTURE_V2.md).

### GPIO

| Function | Count | Direction | Notes |
|---|---|---|---|
| I2C SDA | 1 | Bidirectional | Pressure sensor (400kHz) |
| I2C SCL | 1 | Output | Pressure sensor |
| Pyro 1 enable | 1 | Output | AP2192 EN pin |
| Pyro 2 enable | 1 | Output | AP2192 EN pin |
| Pyro common enable | 1 | Output | Master arm |
| Pyro 1 fault | 1 | Input | AP2192 FLAG, active-low, needs pull-up |
| Pyro 2 fault | 1 | Input | AP2192 FLAG, active-low, needs pull-up |
| Pyro 1 continuity | 1 | Analog in | ADC, 100kΩ pull-up to 3.3V |
| Pyro 2 continuity | 1 | Analog in | ADC, 100kΩ pull-up to 3.3V |
| Buzzer | 1 | Output | GPIO on/off, external tone circuit |
| UART TX | 1 | Output | Telemetry, 115200 baud |
| UART RX | 1 | Input | Optional, for future commands |
| USB D+ | 1 | Bidirectional | Optional, for web interface |
| USB D- | 1 | Bidirectional | Optional, for web interface |
| **Minimum** | **12** | | Without USB |
| **Full** | **14** | | With USB |

### ADC

| Parameter | Requirement | Rationale |
|---|---|---|
| Channels | ≥ 2 | Pyro 1 and Pyro 2 continuity |
| Resolution | ≥ 10 bits | Distinguish open (>3V), good (~50mV), short (0V) |
| Sample rate | ≥ 100 Hz | Two channels at 50Hz each |
| Input range | 0–3.3V | Matches 3.3V logic |

### Peripherals

| Peripheral | Required | Usage |
|---|---|---|
| I2C master | Yes | Pressure sensor (MS5607/BMP280), 400kHz |
| UART | Yes | Telemetry output, 115200 baud |
| Timer | Yes | 50Hz pressure sampling ISR, buzzer pattern ISR |
| ADC | Yes | Pyro continuity sensing |
| USB device | Optional | Web interface, OTA updates, config |
| DMA | Optional | UART TX, I2C transfers (reduces CPU wake time) |
| Second core | Optional | Autonomous pressure sampling alternative to ISR |

### Clock Speed

| Scenario | Minimum | Rationale |
|---|---|---|
| Flight processing | 8 MHz | Process 5 samples in <1ms |
| I2C at 400kHz | 4 MHz | Need ≥10× I2C clock for bit timing |
| USB full-speed | 48 MHz | USB 2.0 FS requires 48MHz reference |
| **Without USB** | **8 MHz** | |
| **With USB** | **48 MHz** | |

### RAM

| Component | Size | Notes |
|---|---|---|
| Flight context | 200 bytes | State variables, no ring buffer (v2.0) |
| Pressure ping-pong | 120 bytes | 2 × 5 samples |
| Telemetry TX queue | 512 bytes | Async UART TX ring (ISR-drained, v2-10) |
| CSV write buffer | 512 bytes | HAL-internal flash buffer |
| Buzzer pattern | 64 bytes | Loaded once per mode |
| Stack | 512 bytes | Interrupt + main |
| **Minimum (no USB)** | **~2 KB** | |
| USB stack | 4–8 KB | TinyUSB or equivalent |
| Network buffers | 8–16 KB | lwIP for USB-ECM |
| **With USB + web** | **~16 KB** | |

### Flash / Program Memory

| Component | Size | Notes |
|---|---|---|
| Flight software | ~20 KB | State machine, filter, pyro logic |
| HAL + drivers | ~10 KB | I2C, UART, timer, ADC |
| Config parser | ~2 KB | X-macro generated |
| Telemetry formatter | ~2 KB | NMEA or binary |
| **Minimum (no USB)** | **~32 KB** | |
| USB stack | ~16 KB | TinyUSB |
| HTTP server + lwIP | ~64 KB | Web interface |
| Web files (littlefs) | ~64 KB | HTML/JS/CSS |
| OTA bootloader | ~8 KB | A/B slot management |
| **With USB + web + OTA** | **~256 KB** | |

### Non-Volatile Storage (Config + Flight Data)

| Component | Size | Notes |
|---|---|---|
| Configuration | ~256 bytes | INI file or raw struct |
| Flight data (1 flight) | ~10 KB | CSV, ~200 samples × 50 bytes |
| Flight data (10 flights) | ~100 KB | If retaining history |
| **Minimum** | **4 KB** | Config + 1 flight, EEPROM or flash sector |
| **Comfortable** | **128 KB+** | Multiple flights, littlefs |

EEPROM is preferred for config (no erase-before-write, wear-leveled).
Flash with filesystem (littlefs) is preferred for flight data.
If no EEPROM, config can be stored in a dedicated flash sector with wear leveling.

## MCU Analysis

### SAMD21 (Microchip, ARM Cortex-M0+, 48MHz)

| Parameter | SAMD21G18A | Requirement | Verdict |
|---|---|---|---|
| Clock | 48 MHz | 48 MHz (USB) | ✅ |
| RAM | 32 KB | 16 KB (with USB) | ✅ Plenty |
| Flash | 256 KB | 256 KB (with USB) | ✅ Exact fit |
| GPIO | 38 (QFN48) | 14 | ✅ |
| ADC | 12-bit, 20 ch | 10-bit, 2 ch | ✅ Exceeds |
| I2C | 6 SERCOM | 1 | ✅ |
| UART | 6 SERCOM | 1 | ✅ |
| USB | Device FS | Device FS | ✅ |
| DMA | 12 channels | Optional | ✅ |
| Timers | 5 TC + 3 TCC | 2 | ✅ |
| EEPROM | Emulated (RWW) | 256 bytes | ⚠️ Flash-based |
| Sleep modes | Idle, Standby | WFE | ✅ |

**Assessment:** Excellent fit. Generous RAM and peripherals. SERCOM flexibility
allows any pin to be I2C/UART/SPI. Arduino Zero ecosystem for prototyping.
No hardware EEPROM but RWW flash section works. The SAMD21 is the closest
drop-in replacement for the RP2040 in terms of capability.

**Suggested implementation:**
- Pressure: TC timer ISR triggers I2C DMA read
- Telemetry: SERCOM UART with DMA TX
- Buzzer: TCC timer with compare match ISR
- USB: Native USB device with TinyUSB
- Config: RWW flash section or last flash page
- Sleep: Idle mode between buffer events

### STM32F072 (ST, ARM Cortex-M0, 48MHz)

| Parameter | STM32F072RBT6 | Requirement | Verdict |
|---|---|---|---|
| Clock | 48 MHz | 48 MHz (USB) | ✅ |
| RAM | 16 KB | 16 KB (with USB) | ⚠️ Tight |
| Flash | 128 KB | 256 KB (with USB) | ❌ Short for web |
| GPIO | 37 (LQFP48) | 14 | ✅ |
| ADC | 12-bit, 16 ch | 10-bit, 2 ch | ✅ |
| I2C | 2 | 1 | ✅ |
| UART | 4 | 1 | ✅ |
| USB | Device FS (no crystal) | Device FS | ✅ |
| DMA | 7 channels | Optional | ✅ |
| Timers | 8 | 2 | ✅ |
| EEPROM | None | 256 bytes | ❌ Flash emulation |
| Sleep modes | Sleep, Stop, Standby | WFE | ✅ |

**Assessment:** Workable for a minimal build without web interface. 16KB RAM
is tight with USB+lwIP but feasible with v2.0 architecture (1.2KB flight
software + ~12KB USB/network). 128KB flash is insufficient for web files —
would need external SPI flash or no web interface. Crystal-less USB is a plus.

**Suggested implementation:**
- Pressure: TIM ISR triggers I2C DMA
- Telemetry: USART with DMA TX
- Buzzer: TIM with output compare ISR
- USB: Native USB (crystal-less), minimal CDC or custom class
- Config: Flash page with HAL_FLASH_Program
- Web interface: Not feasible without external flash
- Sleep: Stop mode with RTC wakeup

**Variant:** STM32F072 with external SPI flash (W25Q128, 16MB) for web files
and flight data. Adds 1 SPI peripheral + 4 pins but solves the storage problem.

### PIC16F1455 (Microchip, 8-bit, 48MHz)

| Parameter | PIC16F1455 | Requirement | Verdict |
|---|---|---|---|
| Clock | 48 MHz (USB) | 48 MHz | ✅ |
| RAM | 1 KB | 2 KB (no USB flight) | ⚠️ Tight |
| Flash | 14 KB | 32 KB (no USB) | ⚠️ Tight |
| GPIO | 11 (14-pin) | 12 (no USB) | ⚠️ Feasible with pin sharing |
| ADC | 10-bit, 5 ch | 10-bit, 2 ch | ✅ |
| I2C | MSSP (1) | 1 | ✅ |
| UART | EUSART (1) | 1 | ✅ |
| USB | Device FS | Device FS | ✅ |
| DMA | None | Optional | ✅ (ISR-driven) |
| Timers | 4 | 2 | ✅ |
| EEPROM | 128 bytes | 256 bytes | ⚠️ Short but workable |
| Sleep modes | Sleep, Idle | WFE | ✅ |

**Revised assessment (v2.0 architecture):** Technically feasible but not recommended.

The v2.0 architecture reduces RAM to ~484 bytes for flight software, leaving
~540 bytes for USB-CDC (Microchip MLA stack needs ~300-400 bytes). This fits
but with ~50 bytes of margin — any growth breaks it.

Flash: v2.0 flight software (~3KB) + USB-CDC stack (~4KB) + HAL (~2KB) +
8-bit math runtime (~1KB) = ~10KB. Fits in 14KB with the free XC8 compiler
but the free compiler's limited optimization may inflate code 30-50%.
The PRO compiler ($75/year) would be needed for a comfortable fit.

GPIO: 11 pins is 1 short of the 12-pin minimum, but sharing the continuity
ADC pin with the pyro fire pin (read before arm, drive after) makes it work.

**Why it doesn't make sense:** The PIC16F1455 costs $0.80 — more than the
STM32C011 ($0.50) which has 6× the RAM, 2× the flash, DMA, and a free
unlimited-optimization compiler. The PIC only wins if an existing PIC
toolchain investment or 14-pin DIP package (hand-soldering) is required.

**If PIC is required:** The PIC32MX270F256B ($2.50, 64KB RAM, 256KB flash,
USB, DMA, 32-bit MIPS) meets all requirements comfortably with no compromises.

## Platform Variants

The flight software is identical across all platforms — only the HAL
implementation changes. This section defines three product variants
and maps each HAL function to the platform-specific implementation.

### Variant Summary

| | MK1B Full | MK1B Reference | MK1B Lite |
|---|---|---|---|
| **MCU** | ESP32-C3 | RP2040 | STM32C011 + CH340E |
| **Config** | WiFi web | USB web | UART serial tool |
| **Telemetry** | UART to radio | UART to radio | UART to radio |
| **Data extraction** | WiFi | USB | UART dump |
| **Pyro channels** | 2 (AP2192) | 2 (AP2192) | 2 (AP2192) |
| **Buzzer** | Yes | Yes | Yes |
| **OTA** | Yes (WiFi) | Yes (USB A/B) | Yes (UART bootloader) |
| **BOM cost** | ~$4.40 | ~$5.00 | ~$3.70 |
| **PCB size** | ~20×25mm | ~20×25mm | ~15×20mm |
| **Use case** | Primary (wireless) | Primary (wired) | Cost-optimized |

### HAL Mapping: MK1B Full (ESP32-C6)

| HAL Function | Implementation |
|---|---|
| `hal_time_ms()` | `esp_timer_get_time() / 1000` |
| `hal_sleep_until_event()` | `esp_light_sleep_start()`, wake on timer |
| `hal_pressure_start()` | `gptimer` ISR triggers `i2c_master_transmit` into ping-pong buffers |
| `hal_pressure_get_buffer()` | Return filled buffer, set by ISR flag |
| `hal_pressure_release_buffer()` | Mark buffer available for ISR |
| `hal_pyro_init()` | `gpio_config()` for EN, FLAG, common enable |
| `hal_pyro_check()` | `adc_oneshot_read()` on 2 channels |
| `hal_pyro_fire()` | `gpio_set_level()` on EN pin |
| `hal_pyro_is_firing()` | Check fire timer state |
| `hal_pyro_fault()` | `gpio_get_level()` on FLAG pin |
| `hal_buzzer_tone_on/off()` | `gpio_set_level()` on buzzer pin |
| `hal_buzzer_task_register()` | Register buzzer async_task_t with task runner |
| `hal_telemetry_send()` | `uart_write_bytes()` with TX DMA, ISR-drained ring |
| `hal_log_start()` | Open LittleFS file, write header, register flush task |
| `hal_log_sample()` | Append formatted CSV line to RAM buffer (non-blocking) |
| `hal_log_stop()` | Signal flush task to finalize and close log file |
| `hal_log_active()` | Return true while log file is open |
| `hal_config_load()` | Read from NVS or SPIFFS, X-macro parser |
| `hal_config_save()` | X-macro serializer, write to NVS or SPIFFS |
| `hal_platform_init()` | WiFi AP init, HTTP server, USB init, UART init |

**Notes:**
- WiFi AP serves the web interface (same HTML/JS as current USB-ECM version)
- USB provides fallback config/data path if WiFi unavailable
- UART1 reserved for telemetry radio (SX1276, future)
- LP core can run pressure sampling autonomously during main core sleep
- ESP-IDF native OTA with automatic rollback

### HAL Mapping: MK1B Lite (STM32C011 + CH340E)

| HAL Function | Implementation |
|---|---|
| `hal_time_ms()` | `SysTick` counter |
| `hal_sleep_until_event()` | `__WFE()`, wake on timer IRQ |
| `hal_pressure_start()` | `TIM` ISR triggers I2C DMA read into ping-pong buffers |
| `hal_pressure_get_buffer()` | Return filled buffer, set by DMA complete IRQ |
| `hal_pressure_release_buffer()` | Mark buffer available |
| `hal_pyro_init()` | `HAL_GPIO_Init()` for EN, FLAG, common enable |
| `hal_pyro_check()` | `HAL_ADC_Start()` on 2 channels |
| `hal_pyro_fire()` | `HAL_GPIO_WritePin()` on EN pin |
| `hal_pyro_is_firing()` | Check fire timer state |
| `hal_pyro_fault()` | `HAL_GPIO_ReadPin()` on FLAG pin |
| `hal_buzzer_tone_on/off()` | `HAL_GPIO_WritePin()` on buzzer pin |
| `hal_buzzer_task_register()` | Register buzzer async_task_t with task runner |
| `hal_telemetry_send()` | `HAL_UART_Transmit_DMA()`, ISR-drained ring |
| `hal_log_start()` | Open flash file, write header, register flush task |
| `hal_log_sample()` | Append formatted CSV line to RAM buffer (non-blocking) |
| `hal_log_stop()` | Signal flush task to finalize and close log file |
| `hal_log_active()` | Return true while log file is open |
| `hal_config_load()` | Read from last flash page, X-macro parser |
| `hal_config_save()` | X-macro serializer, erase + write flash page |
| `hal_platform_init()` | UART init (CH340E provides USB-serial to PC) |

**Notes:**
- CH340E ($0.30, SOP-8) bridges UART to USB — PC sees a serial port
- PC-side Python tool provides config UI over serial
- Flight data downloaded via serial dump command
- No web interface — config via `pyro_config.py` tool
- UART2 reserved for telemetry radio
- OTA via UART bootloader (STM32 built-in)

### Flight Software Compatibility

The same `flight_states.c`, `telemetry.c`, and `buzzer.c` compile for all
three platforms. The flight software does not know which platform it runs
on — it processes pressure buffers, detects events, fires pyros, and emits
telemetry identically on all three.

| Flight software function | Full (ESP32-C3) | Reference (RP2040) | Lite (STM32C011) |
|---|---|---|---|
| `flight_process_samples()` | ✅ | ✅ | ✅ |
| `flight_init()` | ✅ | ✅ | ✅ |
| Pressure filter | ✅ | ✅ | ✅ |
| Apogee detection | ✅ | ✅ | ✅ |
| Pyro firing (all 4 modes) | ✅ | ✅ | ✅ |
| Telemetry events | ✅ async ISR TX | ✅ async ISR TX | ✅ async ISR TX |
| Data logging | ✅ hal_log_sample() | ✅ hal_log_sample() | ✅ hal_log_sample() |
| Buzzer patterns | ✅ async task | ✅ async task | ✅ async task |
| Config loading | WiFi web | USB web | Serial tool |

Tests run against `hal_test.c` which mocks all HAL functions. Since the
flight software is identical, passing the test suite on the host verifies
correctness for all three platforms. Platform-specific testing validates
only the HAL implementation on real hardware.

1. **Verify pin count:** ≥12 GPIO (≥14 with USB)
2. **Verify RAM:** ≥2KB without USB, ≥16KB with USB
3. **Verify flash:** ≥32KB without USB, ≥256KB with USB+web
4. **Implement HAL:** All functions in `hal.h` for the new platform
5. **Pressure sampling:** Timer ISR or DMA at 50Hz into ping-pong buffers
6. **Telemetry TX:** UART with DMA or ISR-driven TX queue
7. **Buzzer:** Timer ISR walks pattern buffer
8. **Config storage:** EEPROM, flash sector, or external storage
9. **Flight data:** Flash filesystem or external SPI flash
10. **Test:** All 99 C tests pass with new HAL (host-compiled)
11. **Verify:** Closed-loop simulation produces correct flight behavior

## Summary

| MCU | RAM | Flash | USB | Web | DMA | Verdict |
|---|---|---|---|---|---|---|
| RP2040 (current) | 264 KB | 2 MB | ✅ | ✅ | ✅ | ✅ Reference platform |
| ESP32-C3 | 400 KB | 4 MB | ✅ | ✅ WiFi | ✅ | ✅ Recommended (Full variant) |
| STM32C011 | 6 KB | 32 KB | ❌ | ❌ | ✅ | ✅ Cost-optimized (Lite variant) |
| SAMD21G18A | 32 KB | 256 KB | ✅ | ✅ | ✅ | ✅ Drop-in alternative |
| STM32F072 | 16 KB | 128 KB | ✅ | ❌ | ✅ | ⚠️ No web, tight RAM |
| PIC16F1455 | 1 KB | 14 KB | ✅ | ❌ | ❌ | ⚠️ Feasible but not recommended |
| PIC32MX270F256B | 64 KB | 256 KB | ✅ | ✅ | ✅ | ✅ If PIC required |

**Active targets (DD-009):** RP2040 (reference), ESP32-C3 (full), STM32C011 (lite).

## Recommended Platform: ESP32-C3

The ESP32-C3 module replaces the MCU and the USB interface with WiFi,
while keeping a UART free for a future telemetry radio (e.g., SX1276).

| Parameter | ESP32-C3-MINI-1 |
|---|---|
| Core | RISC-V, 160MHz |
| RAM | 400 KB |
| Flash | 4 MB (on-module) |
| WiFi | 802.11 b/g/n (pad-side config and data extraction) |
| BLE | 5.0 (pad-side alternative) |
| GPIO | 22 |
| ADC | 12-bit, 6 channels |
| I2C | 1 |
| UART | 2 (one for telemetry radio, one for debug) |
| SPI | 3 (for future SX1276 telemetry radio) |
| DMA | 3 channels |
| Package | 13 × 16.6mm module (includes antenna, flash, crystal) |
| Price | ~$1.50 qty 100 |

### Roles

| Function | Mechanism |
|---|---|
| Flight computer | v2.0 flight software via HAL |
| Pad-side config | WiFi AP, phone connects, web interface |
| Pad-side data extraction | WiFi, download CSV from phone browser |
| Telemetry radio (future) | UART to SX1276/SX1278 module (433/915MHz, out of scope) |

### BOM

| Component | Price (qty 100) |
|---|---|
| ESP32-C3-MINI-1 | $1.50 |
| MS5607 or BMP280 | $2.00 |
| AP2192 | $0.40 |
| 3.3V regulator | $0.15 |
| Passives | $0.05 |
| Buzzer | $0.30 |
| **Total (no radio)** | **$4.40** |
| SX1276 module (future) | ~$3.00 |
| **Total (with radio)** | **~$7.40** |

### Advantages over RP2040

- WiFi replaces USB — no cable on the pad, configure from phone
- Module includes flash, crystal, antenna — fewer external parts
- UART free for telemetry radio
- SPI free for telemetry radio
- 400KB RAM — comfortable for WiFi stack + flight software
- 4MB flash — room for web files, flight data, OTA
- Single module replaces MCU + flash + USB connector + ESD protection

### Pad Workflow

1. Power on rocket on pad
2. Buzzer plays status beeps
3. Connect phone to "PyroMK1B" WiFi network
4. Open web interface in phone browser
5. Verify config, check continuity, review status
6. Disconnect phone, clear the pad
7. Launch

### HAL Implementation Notes

- Pressure: ESP-IDF `i2c_master` with timer ISR into ping-pong buffers
- Telemetry: UART DMA TX to radio connector
- Buzzer: `gptimer` ISR walks pattern buffer
- Web interface: ESP-IDF `httpd` over WiFi AP (replaces lwIP+TinyUSB)
- Config: NVS (non-volatile storage) or SPIFFS/LittleFS
- Sleep: `esp_light_sleep_start()` between buffer events
- OTA: ESP-IDF native OTA with rollback (replaces pico_fota_bootloader)

### Notes

- WiFi and BLE are for pad-side use only (range: ~30m)
- Long-range telemetry requires a separate radio (SX1276/SX1278, 433/915MHz)
- The telemetry radio is out of scope for this document
- The UART telemetry interface in the HAL supports any future radio module

## User Interface Options

The web interface can be delivered to the browser in several ways depending
on the MCU's capabilities. The choice affects flash usage, RAM requirements,
browser compatibility, and multi-device support.

### Option A: On-Device HTTP (current RP2040 implementation)

```
Pyro (serves HTML/JS/CSS + HTTP API) ← USB-ECM network → Browser
```

| Aspect | Detail |
|---|---|
| Browser support | All browsers |
| Flash on pyro | ~64KB (web files) |
| RAM on pyro | ~16KB (lwIP + HTTP + USB network) |
| MCU needs | USB device + network stack |
| Multi-device | One tab per pyro |
| Offline | Yes (pyro serves everything) |
| Suitable for | MK1B Full (RP2040, ESP32-C6) |

### Option B: WebSerial (web UI from internet, serial to pyro)

```
GitHub Pages (HTML/JS/CSS) → Browser → WebSerial API → USB-CDC → Pyro
```

| Aspect | Detail |
|---|---|
| Browser support | Chrome/Edge only (no Safari, no Firefox) |
| Flash on pyro | 0 (no web files) |
| RAM on pyro | ~512 bytes (serial buffer) |
| MCU needs | USB-CDC only |
| Multi-device | One page manages N serial ports |
| Offline | Needs cached PWA or local copy of web files |
| Suitable for | MK1B Lite, MK1B Backup, ground station |

Serial protocol replaces HTTP API:
```
→ STATUS\n
← {"state":"PAD_IDLE","alt_cm":0,...}\n

→ CONFIG\n
← {"pyro1_mode":"delay",...}\n

→ CONFIG pyro2_value=200\n
← OK\n
```

Same JSON payloads — the web UI swaps `fetch()` for `serialPort.write()`.

Ground station multi-device:
```
Browser (ground station UI)
  ├── WebSerial → Pyro #1 (USB-CDC)
  ├── WebSerial → Pyro #2 (USB-CDC via hub)
  └── WebSerial → Ground radio → Pyro #3..N (telemetry)
```

One web page discovers and manages all connected pyros. The same serial
protocol works over USB-CDC (pad-side) and telemetry radio (in-flight).

### Option C: Web Bluetooth (Safari alternative)

```
GitHub Pages (HTML/JS/CSS) → Browser → Web Bluetooth API → BLE → Pyro
```

| Aspect | Detail |
|---|---|
| Browser support | Safari, Chrome, Edge (not Firefox) |
| Flash on pyro | 0 |
| RAM on pyro | BLE stack (~8KB on ESP32) |
| MCU needs | BLE peripheral |
| Multi-device | One page manages N BLE connections |
| Range | ~30m (pad-side only) |
| Suitable for | MK1B Full (ESP32-C3/C6) |

### Option D: Python bridge (universal fallback)

```
GitHub Pages or local HTML → Browser → localhost:8080 → Python tool → Serial → Pyro
```

A small Python script bridges serial to a local HTTP server. The web UI
connects to `http://localhost:8080` and uses the same HTTP API as Option A.
Works on every browser and every OS.

```bash
python3 pyro_bridge.py /dev/tty.usbmodem1234
# Open http://localhost:8080 in any browser
```

| Aspect | Detail |
|---|---|
| Browser support | All browsers |
| Flash on pyro | 0 |
| RAM on pyro | ~512 bytes (serial buffer) |
| MCU needs | USB-CDC or UART |
| Requires | Python 3 on PC |
| Suitable for | All variants, Safari users, automation |

### Recommended Configuration Per Variant

| Variant | Primary UI | Safari Fallback | Ground Station |
|---|---|---|---|
| MK1B Full (ESP32-C3) | WiFi AP (Option A) | Web Bluetooth (C) | WebSerial (B) |
| MK1B Reference (RP2040) | On-device HTTP (Option A) | Python bridge (D) | WebSerial (B) |
| MK1B Lite (STM32C011) | WebSerial (B) | Python bridge (D) | WebSerial (B) |

The MK1B Lite benefits most from WebSerial — it adds a full browser-based
config UI to a $3.70 device with zero flash overhead. The CH340E bridge
($0.30) provides the USB-CDC port.

### Shared Web UI

All options use the same web interface code (HTML/JS/CSS). The transport
layer is abstracted:

```javascript
class PyroTransport {
    async getStatus() { /* implemented by subclass */ }
    async setConfig(cfg) { /* implemented by subclass */ }
}

class HttpTransport extends PyroTransport { /* fetch('/api/status') */ }
class SerialTransport extends PyroTransport { /* port.write('STATUS\n') */ }
class BleTransport extends PyroTransport { /* characteristic.readValue() */ }
class SimTransport extends PyroTransport { /* parent.simApi() */ }
```

The web UI auto-detects the available transport:
1. If `window.parent.simApi` exists → simulation mode
2. If `navigator.serial` available → offer WebSerial
3. If `navigator.bluetooth` available → offer BLE
4. Otherwise → try HTTP (on-device or Python bridge)

## Future Planning

This section evaluates future system capabilities against the v2.0 HAL
interface to confirm they can be implemented without flight software changes
and without new HAL functions.

### Planned Future Capabilities

1. **Shared serial bus** — multiple pyro boards on one UART, coordinated by a bus master
2. **Self-discovery** — pyros identified by factory-unique ID, no user addressing
3. **Ground station** — web UI manages N pyros, assigns roles, validates config
4. **Telemetry radio** — SX1276/SX1278 on 433/915MHz, managed by radio module MCU
5. **Multiplexed UART** — config, telemetry, and bus commands share one serial link
6. **Safe fallback** — unconfigured or corrupted pyro reverts to delay-0 default
7. **Topology change detection** — alert user when devices are added or removed

### v2.0 HAL Compatibility Analysis

Each future capability is mapped to existing v2.0 HAL functions. The flight
software is unchanged — all new behavior lives in the HAL implementation
or in external systems (bus master, ground station, radio module).

| Future Capability | HAL Function Used | Flight Software Change | Notes |
|---|---|---|---|
| Shared serial bus | `hal_telemetry_send()` | None | HAL adds bus framing + address prefix to outbound messages |
| Bus command handling | `hal_config_load/save()` | None | HAL's serial handler parses bus commands, updates config store |
| Self-discovery | `hal_config_load()` | None | HAL reads MCU unique ID, includes in discovery response |
| Unique device ID | `hal_config_load()` | None | UID is a config field populated by HAL from hardware |
| Ground station UI | N/A (external) | None | Runs in browser, talks to bus master, not to flight software |
| Telemetry radio | `hal_telemetry_send()` | None | HAL sends bytes to UART; radio module packetizes externally |
| Multiplexed UART | `hal_telemetry_send()`, `hal_config_load/save()` | None | HAL demuxes inbound: config commands → config store, rest ignored |
| Buzzer coordination | `hal_buzzer_play()` | None | HAL delays pattern start until bus master sends BUZZ command |
| Safe fallback | `hal_config_load()` | None | HAL returns defaults when config is missing or corrupt |
| Topology detection | N/A (external) | None | Bus master tracks discovery responses, alerts ground station |

**Result: zero flight software changes, zero new HAL functions.**

The v2.0 HAL interface is sufficient because:
- `hal_telemetry_send()` is a raw byte transport — bus framing is a HAL concern
- `hal_config_load/save()` is abstract storage — bus-delivered config is transparent
- `hal_buzzer_play()` accepts a pattern — when to start is a HAL decision
- The flight software never addresses other devices, never manages the bus, never knows about the radio

### Bus Architecture

```
Ground Station (browser)
  └── WebSerial or radio link
        └── Bus Master (radio module MCU or USB adapter)
              └── Shared UART (half-duplex)
                    ├── Pyro #1 (uid: A3F7, role: drogue)
                    ├── Pyro #2 (uid: 9C21, role: main)
                    └── Pyro #3 (uid: E104, role: backup)
```

Each pyro is fully autonomous. The bus adds coordination but is never a dependency.

### Discovery Protocol

```
Master → DISCOVER\n
Pyro A → HELLO {"uid":"A3F7","name":"Drogue","role":"delay-0"}\n  (after random 0-50ms delay)
Pyro B → HELLO {"uid":"9C21","name":"","role":"unconfigured"}\n
Pyro C → HELLO {"uid":"E104","name":"","role":"unconfigured"}\n
```

The UID is derived from the MCU's hardware serial number (every MCU has one).
No solder jumpers, no address configuration. The ground station UI shows
discovered devices and guides the user to assign roles.

### Safe Fallback Requirements

- **SYS-SAFE-01**: An unconfigured pyro shall default to delay-0 (drogue at apogee).
- **SYS-SAFE-02**: A pyro with corrupt configuration shall revert to the default.
- **SYS-SAFE-03**: Each pyro shall fly its mission autonomously regardless of bus state.
- **SYS-SAFE-04**: The ground station shall alert the user when device topology changes after configuration.
- **SYS-SAFE-05**: The ground station shall refuse to arm until all devices are configured and validated.

All safe fallback behavior is implemented in `hal_config_load()` (returns defaults
on error) and in the ground station software (external to the pyro). The flight
software always receives a valid `config_t` and flies accordingly.

### Physical Interface

Each pyro board exposes:
- **3.5mm TRRS jack** — pad-side only (config, test, data). Removed before flight.
- **Solder pads** — flight bus (wired-OR shared bus). Soldered on the sled.

All accessories (config cable, ground test plug) connect via the 3.5mm jack.
Inter-board communication uses the soldered flight bus.
See "Physical Interfaces" section above for details.

### Physical Interfaces

The pyro system has two distinct interfaces for different environments.

#### Pad Interface (external, removable): 3.5mm TRRS Jack

For ground-side operations only — removed before flight.

| Contact | Function |
|---|---|
| Tip | TX (pyro → external) |
| Ring 1 | RX (external → pyro) |
| Sleeve | GND |

Accepts:
- **Config cable:** 3.5mm to USB-serial adapter
- **Ground test plug:** ATtiny202 + button, self-powered
- **Direct serial terminal:** any UART adapter

This is the only connector that exits the airframe. It connects to the
bus master or directly to a single pyro for standalone configuration.

#### Flight Bus (internal, soldered): Shared Wired-OR

For inter-board communication on the sled. All connections are soldered
for maximum reliability, but wire failures from handling, vibration,
maintenance, and poor joints are still the primary failure mode.

**Topology: J1708-style shared bus (wired-OR)**

```
Pull-up ─────────────────────────── Bus (single wire)
              │         │         │
           Pyro A    Pyro B    Pyro C
          (stub)    (stub)    (stub)
```

Each device connects to the bus via a short stub wire. TX is open-drain
with a shared pull-up resistor. Collision avoidance by UID priority
(lower UID wins, like CAN/J1708).

**Why shared bus over ring:**

| Topology | Wires (3 devices) | Single break effect | Complexity |
|---|---|---|---|
| Shared bus | 2 (bus + GND) + 3 stubs | Loses one device | Low |
| Single ring | 6 (3 TX pairs) | Loses all downstream | Low |
| Dual ring | 12 (6 TX pairs) | Survives any single break | High, 2 UARTs |

The shared bus has the best fault isolation per wire: a broken stub loses
only that device. A broken backbone loses devices past the break, but the
backbone is short (inches on a sled) and can be made robust with heavier
gauge wire or PCB traces.

**Electrical:**
- Single wire + GND (2 conductors total for the backbone)
- Open-drain TX with 4.7kΩ pull-up to 3.3V
- Each device taps the bus with a short stub (solder or board-to-board)
- No transceiver needed — just an open-drain GPIO
- 9600-115200 baud (short distances, no termination needed)

**Protocol:**
- Master polls: `@A3F7 STATUS\n`
- Device responds: `@A3F7 {"state":"PAD_IDLE",...}\n`
- Broadcast: `DISCOVER\n` → all devices respond with random backoff
- Collision: lower UID wins (devices monitor bus during TX, back off on mismatch)

**Independence:** Every pyro flies its mission autonomously regardless of
bus state. The bus adds coordination (sequenced buzzer, shared radio,
ground station visibility) but is never a dependency for pyro firing.

#### Bus Master Location

The bus master can be:
- **Radio module:** SX1276 + MCU on the sled, bridges bus to ground station
- **Pad controller:** Connected via 3.5mm jack for ground operations
- **Any pyro:** One pyro can be designated master (simplest sled, no extra board)

When the pad controller is connected via the 3.5mm jack, it talks to the
bus master (or directly to a single pyro). The pad controller does not
participate on the flight bus — it's a ground-side device.

### Ground Test and Pad Operations

A ground test button enables pre-flight verification and pad-side operations
without a computer. The ground test plug connects via the 3.5mm jack.

#### Ground Test Plug ($0.40 accessory)

A self-powered 3.5mm plug containing a tiny MCU and a button.
Debounces presses and sends serial commands on TX/RX:

```
Button press     → TEST REPLAY STATUS\n
Button press     → TEST REPLAY ALTITUDE\n
3 presses in 2s  → TEST ARM 1\n
Button press     → TEST FIRE 1\n
```

#### Ground Test Sequence

| Action | Command | Safety |
|---|---|---|
| Replay status beep | TEST REPLAY STATUS | None needed |
| Replay altitude beep | TEST REPLAY ALTITUDE | None needed |
| Arm pyro 1 | TEST ARM 1 | Buzzer warning, 3s auto-disarm |
| Fire pyro 1 | TEST FIRE 1 | Only while armed |
| Arm pyro 2 | TEST ARM 2 | Buzzer warning, 3s auto-disarm |
| Fire pyro 2 | TEST FIRE 2 | Only while armed |

The 3-press arm + 1-press fire prevents accidental firing.

#### Bus Sequenced Ground Test

The bus master sequences so only one pyro fires at a time:

```
Master press 1 → @A3F7 TEST REPLAY STATUS\n   (drogue beeps)
Master press 2 → @9C21 TEST REPLAY STATUS\n   (main beeps)
Master press 3 → @A3F7 TEST ARM 1\n           (arm drogue)
Master press 4 → @A3F7 TEST FIRE 1\n          (fire drogue)
Master press 5 → @9C21 TEST ARM 1\n           (arm main)
Master press 6 → @9C21 TEST FIRE 1\n          (fire main)
```

#### v2.0 HAL Compatibility

| Ground test function | HAL function used | Flight software change |
|---|---|---|
| Replay status beep | `hal_buzzer_play()` | None |
| Replay altitude beep | `hal_buzzer_play()` | None |
| Test fire pyro | `hal_pyro_fire()` | None |
| Button detection | External (serial command) | None |

**Result: zero flight software changes, zero new HAL functions.**

#### Ground Test Plug BOM

| Component | Price |
|---|---|
| ATtiny202 (SOT-23-6) | $0.25 |
| Momentary button | $0.10 |
| 3.5mm TRRS plug | $0.10 |
| Coin cell (CR1220) | $0.15 |
| **Total** | **$0.60** |
