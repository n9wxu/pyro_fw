# Pyro MK1B Flight Computer Specification

## Overview
Dual-function rocket flight computer with data logging and dual pyrotechnic deployment control.

## Hardware
- Raspberry Pi Pico (RP2040)
- Pressure sensor (MS5607-02BA03 or BMP280, auto-detected)
- 2x Pyrotechnic channels with continuity detection
- Buzzer for status indication
- Safe input (active low)
- USB Mass Storage (littlefs with FAT12 emulation)
- UART0 for telemetry output

## Flight States

### 1. PAD_IDLE (Ground Standby)
**Entry:** System startup after safety checks pass
**Behavior:**
- Continuously sample pressure at 100Hz
- Filter and update `ground_level_pressure` (rolling average)
- Monitor for launch detection
- Send telemetry every 1 second via UART0

**Exit Condition:** Altitude increases >10m in <100ms → LAUNCH

### 2. LAUNCH (Boost Phase)
**Entry:** Launch detected
**Behavior:**
- Log pressure/altitude/time data at 10Hz to buffer
- Continue filtering pressure data
- Monitor for apogee detection
- Record timestamp of PAD_IDLE→LAUNCH transition
- Send telemetry every 1 second via UART0

**Exit Condition:** Altitude change <10m in 1 second → APOGEE

### 3. APOGEE (Peak Altitude)
**Entry:** Vertical velocity near zero
**Behavior:**
- Record peak altitude
- Detect descent begins
- Continue logging at 10Hz

**Exit Condition:** Altitude descending → FALLING

### 4. FALLING (Descent Phase)
**Entry:** Altitude decreasing from apogee
**Behavior:**
- Continue logging at 10Hz
- Monitor both pyro deployment conditions continuously
- Fire pyro when condition met
- Record exact conditions at fire time (pressure, altitude, time from launch)
- Send telemetry every 1 second via UART0

**Pyro Deployment Conditions (per channel):**
- **Mode 1:** Distance fallen from apogee (meters)
- **Mode 2:** Altitude above ground level (meters)
- Fire when EITHER condition is met

**Exit Condition:** Both pyros fired → LANDED

### 5. LANDED (Post-Flight)
**Entry:** Both pyros fired
**Behavior:**
- Write all buffered data to CSV file in littlefs
- Close file
- Continue telemetry output
- Remain in this state

## Data Logging

### File Management
- **Filename:** `flight_NNNN.csv` (auto-increment)
- **Location:** littlefs root
- **Behavior:** Delete previous flight file on new flight start

### File Format (CSV)
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
# Launch Time: [timestamp]
# Peak Altitude: [cm]
# Pyro 1 Fire: Time=[ms], Alt=[cm], Pressure=[Pa], Post-fire ADC=[value]
# Pyro 2 Fire: Time=[ms], Alt=[cm], Pressure=[Pa], Post-fire ADC=[value]
Time_ms,Pressure_Pa,Altitude_cm,State
0,101325,0,PAD_IDLE
...
```

### Data Collection
- **Sample Rate:** 
  - PAD_IDLE: 1Hz (1000ms intervals)
  - LAUNCH/APOGEE: 10Hz (100ms intervals)
  - FALLING: 20Hz (50ms intervals) - for precise pyro deployment
  - LANDED: 1Hz (1000ms intervals)
- **Buffer:** 64KB circular buffer in RAM
- **Capacity:** 4,096 samples (16 bytes each)
- **Write:** All buffered data written to CSV file after LANDED state

**Sample Structure:**
```c
struct flight_sample {
    uint32_t time_ms;        // Time since launch (ms)
    int32_t pressure_pa;     // Pressure (Pa)
    int32_t altitude_cm;     // Altitude (cm)
    uint8_t state;           // Flight state
    uint8_t padding[3];      // Alignment
};  // 16 bytes per sample
```

**Buffer Management:**
- Pre-allocated 64KB circular buffer at startup
- **Protected region:** 50 samples before apogee are locked (cannot be overwritten)
- Circular buffer overwrites oldest unprotected data if full
- Typical 3-minute flight: ~1,800 samples at 10Hz + descent at 20Hz = ~40KB

**Deployment Accuracy:**
- 20Hz sampling during FALLING = 50ms intervals
- At 100 m/s ballistic descent: 5m maximum altitude error
- Ensures pyro deployment within 5m of configured setpoint

## Telemetry (UART0)

### Format: Eggtimer-compatible data elements
**Settings:** 115200 baud, 8N1, 1Hz update rate (2 seconds during flight)

### Data Element Structure
Each data element consists of:
- **Trigger byte:** Single ASCII character identifying data type
- **Data:** 1-8 ASCII characters
- **Terminator:** `>` character

### Data Elements

| Element | Trigger | Format | Range | Units | Notes |
|---------|---------|--------|-------|-------|-------|
| Flight Time | `#` | nnnn | 0000-9999 | seconds | Time since launch |
| Altitude/100 | `{` | nnn | 000-999 | feet/100 | 0 to 99,900 ft |
| Flight Phase | `@` | n | 1-9 | state | See phase codes below |
| Velocity | `(` | -nnnn | -9999 to 9999 | fps | After launch only |
| Channel Status | `~` | nn---- | varies | - | Pyro channel status |
| Temperature | `!` | -nnn | -999 to 999 | °C × 10 | 21.1°C = 211 |
| Name/ID | `=` | xxxxxxxx | 8 chars | text | Device identifier |
| Battery Voltage | `?` | nnn | 000-999 | V × 10 | 7.9V = 079 |
| Apogee | `%` | nnnnn | 00000-99999 | feet | After apogee only |
| Max Velocity | `^` | nnnn | 0000-9999 | fps | After apogee only |

### Flight Phase Codes
| Code | Phase | Description |
|------|-------|-------------|
| 1 | WT | Waiting for Launch (PAD_IDLE) |
| 2 | LD | Launch Detected (LAUNCH) |
| 4 | LV | Low Velocity |
| 5 | AP | Nose-Over (APOGEE) |
| 8 | FS | Failsafe (FALLING) |
| 9 | TD | Touchdown (LANDED) |

### Channel Status Format
`~` followed by 6 characters (one per channel, we use 2):
- **Enabled (pre-launch):** `A`, `B`, `C`, `D`, `E`, `F`
- **Disabled:** `-`
- **Fired:** `1`, `2`, `3`, `4`, `5`, `6`

**Examples:**
- `~AB---->` - Channel 1 and 2 enabled, others disabled
- `~1B---->` - Channel 1 fired, Channel 2 enabled
- `~12---->` - Both channels fired

### Example Transmission (Pre-launch)
```
{000>@1>#0000>~AB---->?079>!211>=PYRO001>
```
- `{000>` - Altitude 0 feet
- `@1>` - Waiting for launch
- `#0000>` - Flight time 0 seconds
- `~AB---->` - Channels 1 & 2 enabled
- `?079>` - Battery 7.9V
- `!211>` - Temperature 21.1°C
- `=PYRO001>` - Device ID

### Example Transmission (In Flight)
```
{046>@5>#0018>~1B---->(0213>%04679>^0660>?079>!210>=PYRO001>
```
- `{046>` - Altitude 4600 feet
- `@5>` - Nose-over (apogee)
- `#0018>` - 18 seconds flight time
- `~1B---->` - Channel 1 fired, Channel 2 enabled
- `(0213>` - Velocity 213 fps
- `%04679>` - Apogee 4679 feet
- `^0660>` - Max velocity 660 fps
- `?079>` - Battery 7.9V
- `!210>` - Temperature 21.0°C
- `=PYRO001>` - Device ID

### Transmission Strategy
- **Real-time data:** Altitude, velocity, time (sent once)
- **Status data:** Flight phase, channel status (repeated every transmission)
- **Post-event data:** Apogee, max velocity (repeated after occurrence)
- **Static data:** Name, battery, temperature (repeated every transmission)

### Benefits
- **Robust:** Missing packets don't corrupt subsequent data
- **Simple parsing:** Each element self-contained
- **Compatible:** Works with Eggfinder receivers
- **Efficient:** Only sends relevant data for current flight phase

## Pyrotechnic Control

### Hardware: AP2192 Dual Channel Power Switch
- **Device:** Diodes Inc. AP2192 (1.5A per channel)
- **Features:** Current limiting, thermal limiting, short circuit protection
- **Fault Detection:** Open-drain FLAG outputs (active low)

### Safety Circuit (2-Part Design)
- **GPIO15:** Common enable (must be HIGH to enable pyro circuit)
- **GPIO21:** Pyro 1 enable (AP2192 EN1, HIGH = channel on)
- **GPIO22:** Pyro 2 enable (AP2192 EN2, HIGH = channel on)
- **GPIO17:** Pyro 1 fault (AP2192 FLAG1, LOW = fault detected)
- **GPIO18:** Pyro 2 fault (AP2192 FLAG2, LOW = fault detected)
- **Safe State:** All enable GPIOs LOW

### Fault Detection (AP2192 FLAG Outputs)
The AP2192 FLAG pins indicate:
- **Over-current:** Current limit exceeded
- **Thermal shutdown:** Over-temperature condition
- **Short circuit:** Output shorted to ground
- **Normal:** FLAG pin HIGH (open-drain pulled up)
- **Fault:** FLAG pin LOW (pulled down by IC)

### Fire Sequence
To fire a pyro channel:
1. Set GPIO15 HIGH (enable common)
2. Set GPIO21 or GPIO22 HIGH (enable AP2192 channel)
3. Monitor GPIO17/18 for faults during fire
4. Hold for fire duration (1-2 seconds typical)
5. Set GPIO21/22 LOW (disable channel)
6. Set GPIO15 LOW (disable common)

### Continuity Detection Circuit
Each pyro output has:
- **Pull-up:** 3.3V through 100kΩ resistor
- **ADC Input:** Junction of pyro and pull-up resistor
  - Pyro 1: ADC0 (GPIO26)
  - Pyro 2: ADC1 (GPIO27)

### Continuity Detection Method
**Voltage Divider Analysis:**
- Typical pyro: ~0.5Ω
- Pull-up: 100kΩ
- Expected voltage: V = 3.3V × (0.5Ω / 100000.5Ω) ≈ 0.000016V ≈ 0V
- Short circuit: 0V

**ADC Resolution Enhancement:**
- **Native:** 12-bit (0-4095), ~0.8mV per LSB
- **Oversampling:** Average 256 samples for ~16-bit effective resolution
- **Improved resolution:** ~0.05mV per LSB
- **May distinguish:** 0.5Ω pyro (~16µV) from true short (0V)

**ADC Readings (with oversampling):**
- **Open Circuit:** ADC > 65000 (3.3V, 16-bit scale)
- **Good Pyro (0.5Ω):** ADC ≈ 1-10 (very low but non-zero)
- **Short Circuit:** ADC = 0 (exactly 0V)

**Detection Thresholds (16-bit oversampled):**
- **Open:** ADC > 60000 → Open circuit fault
- **Good:** ADC 1-100 → Good continuity
- **Short:** ADC = 0 → Short circuit fault
- **Uncertain:** ADC 101-59999 → Unexpected, treat as fault

### Fire Success Criteria
A successful fire requires:
1. **Pre-fire:** ADC 1-100 (good continuity, not open or short)
2. **During fire:** FLAG stays HIGH (no fault during fire)
3. **Post-fire:** ADC > 60000 (pyro now open, fired successfully)

### Continuity Check (Startup)
- **Check Timing:** 3 seconds after startup, before PAD_IDLE
- **Safety Critical:** Both pyro channels (GPIO21, GPIO22) must remain LOW throughout test

**Continuity Check Sequence:**
1. Set GPIO21 LOW (Pyro 1 channel off)
2. Set GPIO22 LOW (Pyro 2 channel off)
3. Set GPIO15 LOW (common disabled)
4. **CRITICAL:** Verify GPIO21 and GPIO22 are LOW and will remain LOW
5. Set GPIO15 HIGH (enable common - creates ground path)
6. Wait 10ms for settling
7. Oversample ADC0 (256 samples) - Pyro 1 measurement
8. Oversample ADC1 (256 samples) - Pyro 2 measurement
9. Set GPIO15 LOW (disable common)
10. Analyze both ADC results

**Result Evaluation (per channel):**
- **Open Circuit:** ADC > 60000 → Beep code 2-1 or 3-1
- **Short Circuit:** ADC = 0 → Beep code 2-2 or 3-2
- **Good:** ADC 1-100 → Pass
- **Uncertain:** ADC 101-59999 → Treat as fault

**Safety Note:** 
Both GPIO21 and GPIO22 MUST remain LOW throughout the entire test sequence. Only GPIO15 (common) is toggled HIGH to enable the measurement circuit, then returned LOW.

### Fault Detection (AP2192 FLAG Outputs)
The AP2192 FLAG pins indicate faults **during firing only**:
- **Over-current:** Current limit exceeded
- **Thermal shutdown:** Over-temperature condition
- **Short circuit:** Output shorted to ground
- **Normal:** FLAG pin HIGH (open-drain pulled up)
- **Fault:** FLAG pin LOW (pulled down by IC)

**FLAG Monitoring:**
- Monitor GPIO17/18 continuously during fire sequence
- If FLAG goes LOW during fire: abort and record fault

### Configuration (per channel)
```c
struct pyro_config {
    uint8_t mode;        // 1=fallen from apogee, 2=AGL
    int32_t distance_m;  // Deployment distance in meters
};
```

### Manual Test Mode (Safe Input)
- **Input:** Active low, internal pull-up
- **Pyro 1 Test:** Hold low for 3 seconds
- **Pyro 2 Test:** Release, then hold low for 3 seconds again

## Status Indication (Buzzer)

### Beep Code Format
- **Structure:** 2 digits, each digit 1-5
- **Digit:** N beeps (100ms on, 200ms off)
- **Between digits:** 300ms pause
- **Between codes:** 500ms pause
- **Repeat:** After 1 second

### Beep Codes
| Code | Meaning |
|------|---------|
| 1-1  | All systems good |
| 2-1  | Pyro 1 open circuit (pre-flight) |
| 2-2  | Pyro 1 short circuit (pre-flight) |
| 2-3  | Pyro 1 fault during fire (AP2192 FLAG) |
| 2-4  | Pyro 1 failed to open after fire |
| 3-1  | Pyro 2 open circuit (pre-flight) |
| 3-2  | Pyro 2 short circuit (pre-flight) |
| 3-3  | Pyro 2 fault during fire (AP2192 FLAG) |
| 3-4  | Pyro 2 failed to open after fire |
| 4-1  | Pressure sensor failure |
| 4-2  | Filesystem failure |
| 5-5  | Critical system failure |

### Startup Sequence
1. Power on
2. Initialize peripherals (all pyro GPIOs LOW)
3. Detect pressure sensor
4. **Wait 3 seconds** (allow system to stabilize)
5. **Check pyro continuity** (single test, both channels measured)
   - GPIO21, GPIO22 remain LOW throughout
   - GPIO15 pulsed HIGH for measurement
   - Both ADC0 and ADC1 sampled
6. Beep status code
7. If good (1-1): Load config and enter PAD_IDLE
8. If failure: Repeat failure code continuously, do not enter PAD_IDLE

## Configuration Storage
- **Location:** `config.ini` in littlefs root
- **Format:** INI file with key-value pairs
- **Behavior:** If file missing, create with defaults

### config.ini Format
```ini
[pyro]
id=PYRO001
name=Test Rocket
pyro1_mode=1
pyro1_distance=300
pyro2_mode=2
pyro2_distance=500
```

### Configuration Parameters
| Parameter | Type | Description | Default |
|-----------|------|-------------|---------|
| id | string | Unique device identifier | PYRO001 |
| name | string | Rocket/flight name | Default |
| pyro1_mode | int | 1=fallen from apogee, 2=AGL | 1 |
| pyro1_distance | int | Distance in meters | 300 |
| pyro2_mode | int | 1=fallen from apogee, 2=AGL | 2 |
| pyro2_distance | int | Distance in meters | 500 |

### Startup Behavior
1. Attempt to read `config.ini` from littlefs
2. If file exists: Parse and load configuration
3. If file missing: Use defaults and write `config.ini` to filesystem
4. Configuration loaded before entering PAD_IDLE state

## Altitude Calculation
- **Reference:** `ground_level_pressure` established in PAD_IDLE
- **Formula:** Barometric formula (integer math)
- **Units:** Centimeters (cm)

## Safety Features
1. **Continuity check** before flight
2. **Manual test mode** with safe input
3. **Ground level calibration** in PAD_IDLE
4. **Dual deployment conditions** (redundancy)
5. **Data logging** for post-flight analysis
6. **Status beep codes** for field diagnostics

## Pin Assignments

### I2C (Pressure Sensor)
- **GPIO 8:** I2C0 SDA
- **GPIO 9:** I2C0 SCL

### UART (Telemetry)
- **GPIO 0:** UART0 TX
- **GPIO 1:** UART0 RX

### Pyrotechnic Control
- **GPIO 15:** Common enable (HIGH = circuit enabled)
- **GPIO 21:** Pyro 1 enable (AP2192 EN1)
- **GPIO 22:** Pyro 2 enable (AP2192 EN2)

### Pyrotechnic Fault Detection (AP2192)
- **GPIO 17:** Pyro 1 fault (AP2192 FLAG1, active low)
- **GPIO 18:** Pyro 2 fault (AP2192 FLAG2, active low)

### Pyrotechnic Continuity Sense (ADC)
- **GPIO 26 (ADC0):** Pyro 1 continuity voltage
- **GPIO 27 (ADC1):** Pyro 2 continuity voltage

### User Interface
- **GPIO TBD:** Safe input (active low, internal pull-up)
- **GPIO TBD:** Buzzer (PWM)

### Status LED
- **GPIO 25:** Onboard LED (Pico)

## State Transition Diagram
```
STARTUP → SAFETY_CHECK → [GOOD] → PAD_IDLE
                       ↓
                    [FAIL] → ERROR (beep loop)

PAD_IDLE → [+10m in <100ms] → LAUNCH
LAUNCH → [<10m change in 1s] → APOGEE
APOGEE → [descending] → FALLING
FALLING → [both pyros fired] → LANDED
```

## Open Questions
1. Maximum flight duration / buffer size?
2. Pyro fire pulse duration?
3. Pyro fire current/voltage requirements?
4. Exact continuity resistance thresholds?
5. Device ID generation/storage method?
6. Configuration UI (USB serial commands)?
