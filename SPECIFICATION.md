# Pyro MK1B Flight Computer Specification

## AI Restart Summary

This section contains everything needed to resume development with a new AI session.

### Project Overview
Dual-deployment rocket flight computer on Raspberry Pi Pico (RP2040). Logs flight data to littlefs flash, presents files via USB Mass Storage (FAT12 emulation), fires two pyrotechnic channels for parachute deployment, outputs Eggtimer-compatible telemetry via UART.

### Current Implementation Status
- **Flight controller**: `src/flight_controller.c` - NEEDS OVERHAUL per this spec
- **fat_mimic library**: `lib/fat_mimic/` - COMPLETE. Writable FAT12 over littlefs. See `lib/fat_mimic/README.md`
- **Pressure sensors**: `src/pressure_sensor.c`, `src/ms5607_driver.c`, `src/bmp280_driver.c` - COMPLETE
- **littlefs driver**: `src/littlefs_driver.c` - COMPLETE
- **USB MSC driver**: `src/usb_msc_driver.c` - COMPLETE
- **Config parser**: NOT IMPLEMENTED. config.ini created with defaults but not parsed
- **Beep codes**: NOT IMPLEMENTED. Buzzer on GPIO 16
- **CSV flight logging**: NOT IMPLEMENTED. Buffer exists, file write not done
- **Test mode**: NOT IMPLEMENTED

### Build
```bash
mkdir -p build && cd build && cmake -G Ninja .. && ninja
# Flash: copy build/pyro_fw_c.uf2 to RPI-RP2 drive
```
Dependencies: littlefs v2.11.2, Unity v2.6.0 (FetchContent).

### Key Design Decisions
1. Integer-only math everywhere
2. No flash I/O in USB callbacks (prevents RP2040 XIP/DMA panics)
3. Apogee is an event, not a state (4 states total)
4. T=0 backdated to first motion, not detection threshold
5. GPIO 8 dual-use: jumper check at boot before I2C init
6. All pyro modes available on both channels
7. File deletions only on USB unmount
8. State machine uses switch dispatch with default→PAD_IDLE (no function pointer array)

### Hardware Pins
| GPIO | Function | Notes |
|------|----------|-------|
| 0 | UART0 TX | Telemetry, 115200 baud |
| 1 | UART0 RX | |
| 8 | Test input / I2C0 SDA | Active low, pull-up; digital at boot, then I2C |
| 9 | I2C0 SCL | Pressure sensor, 400kHz |
| 15 | Pyro common enable | HIGH = enabled |
| 16 | Buzzer | PWM, 3kHz square wave |
| 17 | Pyro 1 fault | AP2192 FLAG1, active low |
| 18 | Pyro 2 fault | AP2192 FLAG2, active low |
| 21 | Pyro 1 enable | AP2192 EN1 |
| 22 | Pyro 2 enable | AP2192 EN2 |
| 25 | Onboard LED | |
| 26 | Pyro 1 continuity | ADC0, 100k pull-up |
| 27 | Pyro 2 continuity | ADC1, 100k pull-up |

### Memory
```
Flash: 2MB (firmware ~90KB, littlefs ~680KB)
RAM: 264KB (flight buffer 64KB, fat_mimic 35KB, stack/heap 165KB)
```

---

## Flight States

```
STARTUP → TEST_MODE (if GPIO 8 jumper present)
        → PAD_IDLE (normal boot)

PAD_IDLE ──[+10m in <1s]──→ ASCENT
ASCENT ──[apogee event]──→ DESCENT
DESCENT ──[altitude stable <1m for 1s]──→ LANDED
```

### Test Mode
- Entry: GPIO 8 test input LOW at boot (jumper to ground)
- Beep test pattern, signal continuity status
- Stay until jumper removed
- On removal: 10s countdown → fire pyro 1 (if good) → 5s → fire pyro 2 (if good)
- GPIO 8 reconfigured for I2C only after normal boot proceeds
- After landing, short press of test button triggers altitude beep-out

### PAD_IDLE
- 100Hz sampling, ground pressure calibration
- Ascent trigger: +10m altitude in <1 second
- On trigger: backdate T=0 by scanning buffer to first positive altitude change

### ASCENT
- 10Hz sampling
- Log `under_thrust` boolean (speed increasing = motor burn)
- Arm pyros when vertical speed < 10 m/s (1000 cm/s)
- Apogee event: armed AND altitude change <= 0
- Record peak altitude and apogee time
- Fire pyros per config, transition to DESCENT
- Post-fire diagnostics (~1s after fire):
  - BAD_PARACHUTE: continuity open + acceleration high (chute failed)
  - BAD_PYRO: continuity closed + acceleration high (pyro failed)
  - Keep re-firing if acceleration high

### DESCENT
- 20Hz sampling
- Check pyro conditions every sample
- Landing: altitude change < 1m for 1 second
- Compute flight time from backdated T=0

### LANDED
- 1Hz sampling
- Write flight CSV, beep max altitude, continue telemetry
- Altitude beep units from config: cm, m, or ft. Beep mode: digits (each digit) or hundreds (value / 100).
- Short press of test button (GPIO 8) triggers altitude beep-out repeat

## Pyro Firing Modes (both channels)

| Mode | Config | Parameter | Unit | Condition |
|------|--------|-----------|------|-----------|
| 1 | fallen | distance | meters | max_alt - current_alt >= value |
| 2 | agl | altitude | meters | current_alt <= value |
| 3 | speed | speed | m/s | downward speed >= value |
| 4 | delay | time | seconds | time since apogee >= value |

Only fire if pre-flight continuity good. Non-blocking 500ms pulse.

## Vertical Speed
```c
vertical_speed_cms = (altitude_cm - prev_altitude_cm) * 1000 / dt_ms;
```

## Configuration (config.ini)
```ini
[pyro]
id=PYRO001
name=My Rocket
pyro1_mode=delay
pyro1_value=0
pyro2_mode=agl
pyro2_value=300
units=m
beep_mode=digits
```

| Parameter | Values | Default |
|-----------|--------|---------|
| id | 8 chars | PYRO001 |
| name | 8 chars | My Rocket |
| pyro1_mode | fallen/agl/speed/delay | delay |
| pyro1_value | 0-9999 | 0 |
| pyro2_mode | fallen/agl/speed/delay | agl |
| pyro2_value | 0-9999 | 300 |
| units | cm/m/ft | m |
| beep_mode | digits/hundreds | digits |

All internal calculations use cm. Pyro values and altitude reporting use the selected units. Speed mode uses units/second (cm/s, m/s, or ft/s).

## Flight Sample (16 bytes)
```c
typedef struct {
    uint32_t time_ms;
    int32_t  pressure_pa;
    int32_t  altitude_cm;
    uint8_t  state;
    uint8_t  under_thrust;
    uint8_t  padding[2];
} flight_sample_t;
```

## Telemetry Phase Codes
| Code | Phase | State |
|------|-------|-------|
| 1 | WT | PAD_IDLE |
| 2 | LD | ASCENT (thrust) |
| 4 | LV | ASCENT (coasting) |
| 5 | AP | Apogee event |
| 8 | FS | DESCENT |
| 9 | TD | LANDED |

## Beep Codes
| Code | Meaning |
|------|---------|
| 1-1 | All good |
| 2-1 | Pyro 1 open |
| 2-2 | Pyro 1 short |
| 3-1 | Pyro 2 open |
| 3-2 | Pyro 2 short |
| 4-1 | Sensor failure |
| 4-2 | Filesystem failure |
| 5-5 | Critical failure |

Altitude beep-out: digit by digit, zero = long beep, 2s pause between reps.

## Continuity Thresholds (16-bit oversampled ADC)
| ADC | Condition |
|-----|-----------|
| > 60000 | Open circuit |
| 1-100 | Good |
| 0 | Short circuit |
