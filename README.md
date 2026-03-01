# pyro_fw
Firmware for the Pyro MK1B Rocket Flight Computer

## Features
- **Dual Pyrotechnic Control** - AP2192 power switches with fault detection
- **Flight Data Logging** - 10Hz CSV logging to littlefs
- **Real-time Telemetry** - 1Hz JSON output via UART
- **Pressure Sensing** - MS5607-02BA03 or BMP280 (auto-detected)
- **Altitude Calculation** - Integer-only barometric formula
- **Continuity Checking** - ADC oversampling for pre-flight verification
- **USB Mass Storage** - littlefs with FAT12 emulation
- **Status Beep Codes** - Field-diagnosable error reporting
- **Configurable Deployment** - INI file configuration

## Hardware
- **MCU:** Raspberry Pi Pico (RP2040)
- **Pressure Sensor:** MS5607-02BA03 or BMP280 (auto-detected)
- **Pyro Control:** AP2192 dual channel power switch (1.5A per channel)
- **Storage:** littlefs on Pico flash (1.8MB)
- **Interfaces:** USB, UART0, I2C0

## Flight Modes
1. **PAD_IDLE** - Ground calibration, launch detection (+10m in <100ms)
2. **LAUNCH** - Boost phase, 10Hz data logging
3. **APOGEE** - Peak altitude detection (<10m change in 1s)
4. **FALLING** - Descent, pyro deployment monitoring
5. **LANDED** - Post-flight data write and telemetry

## Configuration
Edit `config.ini` on the USB drive (appears when connected to PC):

```ini
[pyro]
id=PYRO001
name=My Rocket
pyro1_mode=1
pyro1_distance=300
pyro2_mode=2
pyro2_distance=500
```

**Pyro Modes:**
- **Mode 1:** Distance fallen from apogee (meters)
- **Mode 2:** Altitude above ground level (meters)

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
- **GPIO TBD:** Buzzer (PWM)
- **GPIO TBD:** Safe input (active low, manual test)

## Data Logging
Flight data saved to `flight_NNNN.csv` with:
- **Metadata:** ID, name, config, continuity test results
- **Flight Data:** 10Hz pressure/altitude/time/state
- **Events:** Launch time, peak altitude, pyro deployments
- **Verification:** Post-fire ADC readings

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
**Format:** JSON, 115200 baud, 1Hz
```json
{
  "state": "LAUNCH",
  "time_ms": 1234,
  "pressure_pa": 95000,
  "altitude_cm": 54500,
  "ground_pressure_pa": 101325,
  "pyro1_fired": false,
  "pyro2_fired": false
}
```

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
