# pyro_fw
Firmware for the Pyro MK1B Rocket Flight Computer

## Features
- **Dual Pyrotechnic Control** - AP2192 power switches with fault detection
- **Four Firing Modes** - Fallen distance, AGL altitude, descent speed, timed delay
- **Flight Data Logging** - 10-20Hz CSV logging to littlefs
- **Real-time Telemetry** - 1Hz Eggtimer-compatible output via UART
- **Pressure Sensing** - MS5607-02BA03 or BMP280 (auto-detected)
- **Altitude Calculation** - Integer-only barometric formula
- **Continuity Checking** - ADC oversampling for pre-flight verification
- **USB Mass Storage** - Writable FAT12 over littlefs ([fat_mimic library](lib/fat_mimic/README.md))
- **Test Mode** - GPIO 8 jumper for ground pyro testing
- **Status Beep Codes** - Field-diagnosable error reporting
- **Altitude Beep-out** - Max altitude announced after landing (m/ft/ft100)
- **Configurable Deployment** - INI file configuration via USB

## Hardware
- **MCU:** Raspberry Pi Pico (RP2040)
- **Pressure Sensor:** MS5607-02BA03 or BMP280 (auto-detected)
- **Pyro Control:** AP2192 dual channel power switch (1.5A per channel)
- **Storage:** littlefs on Pico flash (1.8MB)
- **Interfaces:** USB, UART0, I2C0

## Flight States
1. **PAD_IDLE** - Ground calibration, ascent detection (+10m in <1s)
2. **ASCENT** - Boost/coast phase, 10Hz logging, apogee detection
3. **DESCENT** - Pyro deployment, 20Hz logging, landing detection
4. **LANDED** - Post-flight data write, altitude beep-out

Apogee is an event within ASCENT, not a separate state. Pyros arm only after vertical speed drops below 10 m/s.

## Pyro Modes
| Mode | Description |
|------|-------------|
| fallen | Distance fallen from apogee (in configured units) |
| agl | Altitude above ground level (in configured units) |
| speed | Downward vertical speed threshold (configured units/second) |
| delay | Seconds after apogee event |

## Configuration
Edit `config.ini` on the USB drive (appears when connected to PC):

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

**Pyro Modes:**
- **fallen:** Distance fallen from apogee (in configured units)
- **agl:** Altitude above ground level (in configured units)
- **speed:** Downward vertical speed threshold (configured units/second)
- **delay:** Seconds after apogee event

**Units:** `cm` (centimeters), `m` (meters), `ft` (feet) - applies to pyro values and altitude reporting

**Beep Mode:** `digits` (each digit beeped) or `hundreds` (value / 100, then each digit)

**Defaults:** If `config.ini` is missing, defaults are created automatically.

## Status Beep Codes
Two-digit codes (1-5 beeps per digit):
- 100ms beep, 200ms pause within digit
- 300ms pause between digits
- 500ms pause between codes
- Repeats after 1 second

| Code | Meaning |
|------|---------|
| 1-1  | All systems good ✓ |
| 2-1  | Pyro 1 open circuit |
| 2-2  | Pyro 1 short circuit |
| 2-3  | Pyro 1 fault during fire |
| 2-4  | Pyro 1 failed to open after fire |
| 3-1  | Pyro 2 open circuit |
| 3-2  | Pyro 2 short circuit |
| 3-3  | Pyro 2 fault during fire |
| 3-4  | Pyro 2 failed to open after fire |
| 4-1  | Pressure sensor failure |
| 4-2  | Filesystem failure |
| 5-5  | Critical system failure |

## Pin Assignments
### Communication
- **GPIO 0:** UART0 TX (telemetry output)
- **GPIO 1:** UART0 RX
- **GPIO 8:** I2C0 SDA (pressure sensor)
- **GPIO 9:** I2C0 SCL (pressure sensor)

### Pyrotechnic Control
- **GPIO 15:** Common enable (master switch)
- **GPIO 21:** Pyro 1 enable (AP2192 EN1)
- **GPIO 22:** Pyro 2 enable (AP2192 EN2)
- **GPIO 17:** Pyro 1 fault (AP2192 FLAG1, active low)
- **GPIO 18:** Pyro 2 fault (AP2192 FLAG2, active low)
- **GPIO 26 (ADC0):** Pyro 1 continuity sense
- **GPIO 27 (ADC1):** Pyro 2 continuity sense

### User Interface
- **GPIO 25:** Onboard LED (status)
- **GPIO 16:** Buzzer (PWM, 3kHz square wave)
- **GPIO 8:** Test input (active low, internal pull-up; also I2C0 SDA after boot)

## Data Logging
Flight data saved to `flight_NNNN.csv` with:
- **Metadata:** ID, name, config, continuity test results
- **Flight Data:** Variable rate pressure/altitude/time/state
  - PAD_IDLE: 100Hz (10ms - quick launch detection)
  - LAUNCH/APOGEE: 10Hz
  - FALLING: 20Hz (for precise pyro deployment, ±5m accuracy)
  - LANDED: 1Hz
- **Events:** Launch time, peak altitude, pyro deployments
- **Verification:** Post-fire ADC readings
- **Buffer:** 64KB RAM (4,096 samples, ~6.8 minutes capacity)

### Example CSV Header
```
# Pyro ID: PYRO001
# Name: Test Rocket
# Flight Configuration:
# Pyro 1 Mode: 1 (fallen), Distance: 300m
# Pyro 2 Mode: 2 (AGL), Distance: 500m
# Pre-flight Continuity Test:
# Pyro 1 ADC: 45 (Good)
# Pyro 2 ADC: 52 (Good)
# Ground Pressure: 101325 Pa
# Launch Time: 0ms
# Peak Altitude: 120000 cm
# Pyro 1 Fire: Time=1234ms, Alt=90000cm, Pressure=95000Pa, Post-fire ADC=65000
# Pyro 2 Fire: Time=2345ms, Alt=50000cm, Pressure=98000Pa, Post-fire ADC=65100
Time_ms,Pressure_Pa,Altitude_cm,State
0,101325,0,PAD_IDLE
...
```

## Telemetry (UART0)
**Format:** Eggtimer-compatible data elements, 115200 baud, 1Hz

### Data Element Structure
`<trigger><data>>`

**Example transmission:**
```
{046>@5>#0018>~1B---->(0213>%04679>^0660>?079>!210>=PYRO001>
```

### Key Data Elements
| Trigger | Data | Description |
|---------|------|-------------|
| `{` | nnn | Altitude in hundreds of feet (046 = 4600 ft) |
| `@` | n | Flight phase (1=WT, 2=LD, 5=AP, 8=FS, 9=TD) |
| `#` | nnnn | Flight time in seconds |
| `~` | nn---- | Channel status (A/B=enabled, 1/2=fired, -=disabled) |
| `(` | -nnnn | Velocity in fps |
| `%` | nnnnn | Apogee in feet (after apogee only) |
| `^` | nnnn | Max velocity in fps (after apogee only) |
| `?` | nnn | Battery voltage × 10 (079 = 7.9V) |
| `!` | -nnn | Temperature × 10 °C (210 = 21.0°C) |
| `=` | xxxxxxxx | Device name/ID (8 chars) |

**Channel Status Examples:**
- `~AB---->` - Both channels enabled (pre-launch)
- `~1B---->` - Channel 1 fired, Channel 2 enabled
- `~12---->` - Both channels fired

**Benefits:** Compatible with Eggfinder receivers, robust against packet loss

## Safety Features
1. **Pre-flight Continuity Check** - ADC oversampling (256 samples, 16-bit effective)
2. **Two-part Pyro Safety** - Common enable + channel enable
3. **AP2192 Protection** - Hardware current limiting, thermal shutdown, short circuit protection
4. **Fault Monitoring** - FLAG pins monitored during fire
5. **Post-fire Verification** - ADC confirms pyro opened successfully
6. **Manual Test Mode** - Safe input for ground testing

## Continuity Detection
- **Circuit:** 100kΩ pull-up to 3.3V on each pyro output
- **Method:** ADC oversampling (256 samples) for 16-bit effective resolution
- **Thresholds:**
  - Open: ADC > 60000 (3.3V)
  - Good: ADC 1-100 (very low but non-zero)
  - Short: ADC = 0 (exactly 0V)

## Building
Requires Pico SDK 2.2.0 or later:
```bash
mkdir build && cd build
cmake ..
make
```

Flash `pyro_fw_c.uf2` to Pico in bootloader mode (hold BOOTSEL while connecting USB).

## Usage
1. **Power on** - System initializes
2. **Wait 3 seconds** - Continuity check performed
3. **Listen for beeps** - 1-1 = good, others = fault
4. **Connect USB** - Edit `config.ini` if needed
5. **Arm system** - Place in rocket
6. **Launch** - Automatic detection and logging
7. **Retrieve** - Download `flight_NNNN.csv` from USB drive

## Development Status
See [SPECIFICATION.md](SPECIFICATION.md) for detailed requirements and implementation notes.

## License
See LICENSE file for details.
