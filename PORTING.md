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
| Telemetry TX queue | 256 bytes | Async UART output |
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
| RAM | 1 KB | 2 KB (no USB flight) | ❌ |
| Flash | 14 KB | 32 KB (no USB) | ❌ |
| GPIO | 11 (14-pin) | 12 (no USB) | ❌ |
| ADC | 10-bit, 5 ch | 10-bit, 2 ch | ✅ |
| I2C | MSSP (1) | 1 | ✅ |
| UART | EUSART (1) | 1 | ✅ |
| USB | Device FS | Device FS | ✅ |
| DMA | None | Optional | ❌ |
| Timers | 4 | 2 | ✅ |
| EEPROM | 128 bytes | 256 bytes | ⚠️ Short |
| Sleep modes | Sleep, Idle | WFE | ✅ |

**Assessment:** Not feasible. 1KB RAM cannot hold even the v2.0 flight context
(200 bytes) plus stack, buffers, and any USB operation. 14KB flash is too small
for the flight software alone (~20KB). 11 GPIO pins are 1 short of the minimum
12 without USB. 8-bit architecture with no DMA means the CPU cannot sleep during
I2C transfers. The PIC16F1455 is designed for simple USB peripherals (HID, CDC),
not flight computers.

**If PIC is required:** Consider PIC32MX270F256B (32-bit, 256KB flash, 64KB RAM,
USB, DMA) which meets all requirements comfortably.

## Porting Checklist

1. **Verify pin count:** ≥12 GPIO (≥14 with USB)
2. **Verify RAM:** ≥2KB without USB, ≥16KB with USB
3. **Verify flash:** ≥32KB without USB, ≥256KB with USB+web
4. **Implement HAL:** All functions in `hal.h` for the new platform
5. **Pressure sampling:** Timer ISR or DMA at 50Hz into ping-pong buffers
6. **Telemetry TX:** UART with DMA or ISR-driven TX queue
7. **Buzzer:** Timer ISR walks pattern buffer
8. **Config storage:** EEPROM, flash sector, or external storage
9. **Flight data:** Flash filesystem or external SPI flash
10. **Test:** All 64 C tests pass with new HAL (host-compiled)
11. **Verify:** Closed-loop simulation produces correct flight behavior

## Summary

| MCU | RAM | Flash | USB | Web | DMA | Verdict |
|---|---|---|---|---|---|---|
| RP2040 (current) | 264 KB | 2 MB | ✅ | ✅ | ✅ | ✅ Reference platform |
| SAMD21G18A | 32 KB | 256 KB | ✅ | ✅ | ✅ | ✅ Best alternative |
| STM32F072 | 16 KB | 128 KB | ✅ | ❌ | ✅ | ⚠️ No web, tight RAM |
| PIC16F1455 | 1 KB | 14 KB | ✅ | ❌ | ❌ | ❌ Insufficient |
| PIC32MX270F256B | 64 KB | 256 KB | ✅ | ✅ | ✅ | ✅ If PIC required |

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
