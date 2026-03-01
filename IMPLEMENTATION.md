# Flight Controller Implementation

## Files Created

### flight_controller.c
Minimal flight computer firmware implementing:

**Core Features:**
- 5-state flight state machine (PAD_IDLE → LAUNCH → APOGEE → FALLING → LANDED)
- Variable sample rate (1Hz/10Hz/20Hz based on state)
- 64KB circular buffer with apogee protection
- Eggtimer-compatible telemetry output
- Dual pyro channel control with safety
- ADC oversampling (256x) for continuity check
- Barometric altitude calculation

**State Machine:**
- PAD_IDLE: Ground calibration, 1Hz sampling
- LAUNCH: Detect +10m altitude, 10Hz sampling, track max altitude
- APOGEE: Detect altitude drop >10m, protect last 50 samples
- FALLING: 20Hz sampling, pyro deployment logic (±5m accuracy)
- LANDED: Detect <5m altitude with <1m change

**Pyro Deployment:**
- Mode 1: Distance fallen from apogee (meters)
- Mode 2: Altitude AGL (meters)
- Fire when EITHER condition met
- 500ms fire duration

**Telemetry Format (Eggtimer):**
```
{046>@5>#0018>~1B---->%04679>=PYRO001>
```
- `{nnn>` - Altitude (hundreds of feet)
- `@n>` - Flight phase (1=WT, 2=LD, 5=AP, 8=FS, 9=TD)
- `#nnnn>` - Flight time (seconds)
- `~nn---->` - Channel status (A/B=enabled, 1/2=fired)
- `%nnnnn>` - Apogee (feet, after apogee)
- `=xxxxxxxx>` - Device name

**Buffer Management:**
- 4,096 samples × 16 bytes = 64KB
- Circular buffer overwrites oldest data
- 50 samples before apogee are protected
- Typical 3-minute flight: ~40KB

## Build Configuration

### CMakeLists.txt
Simplified build:
- Removed USB/littlefs/PIO dependencies
- Added hardware_adc and hardware_uart
- Minimal executable with pressure sensor drivers

## What's Missing (Future Work)

1. **Config file parser** - Currently uses hardcoded defaults
2. **CSV file writer** - Data logging to filesystem
3. **Beep code generator** - Status indication
4. **Fault detection** - AP2192 FLAG pin monitoring
5. **Post-fire verification** - ADC check after pyro fire
6. **Velocity calculation** - For telemetry (currently 0)
7. **Max velocity tracking** - For telemetry
8. **Safe input handler** - Manual pyro test mode

## Testing Requirements

Before flight:
1. Verify continuity check thresholds (Open>60000, Good 1-100, Short=0)
2. Test pyro fire sequence with LED/resistor load
3. Verify state transitions with altitude simulation
4. Check telemetry output format
5. Validate buffer protection logic
6. Test 20Hz sampling during FALLING state

## Build Instructions

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cd pyro_fw
mkdir -p build && cd build
cmake ..
make
```

Flash `pyro_fw_c.uf2` to Pico in BOOTSEL mode.

## Code Size Estimate

- Core logic: ~2KB
- Pressure drivers: ~3KB
- Buffer: 64KB (RAM)
- Total flash: ~5KB
- Total RAM: ~66KB (buffer + stack + globals)

Plenty of headroom on RP2040 (2MB flash, 264KB RAM).
