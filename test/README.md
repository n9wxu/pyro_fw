# Unit Tests

Host-compiled unit tests for the Pyro MK1B flight state machine and telemetry.

## Running

```bash
cd build
ninja host_tests
```

Tests compile with the host compiler (not ARM cross-compiler) using mock
hardware implementations. They run as part of the GitHub Actions CI build.

## Files

| File | Purpose |
|------|---------|
| `test_flight_states.c` | Tests for boot sequence, flight states, helpers, telemetry |
| `mocks.h` | Mock declarations for Pico SDK, sensors, pyro, UART, LFS |
| `mocks.c` | Mock implementations with configurable state |

## Test Coverage

- **Helpers:** pressure_to_altitude_cm, filter_pressure, buf_add
- **Boot:** full sequence to PAD_IDLE, calibration, no-sensor handling, I2C settle timing
- **PAD_IDLE:** ground stability, ascent detection, continuity checking
- **ASCENT:** max altitude tracking, pyro arming, apogee detection
- **DESCENT:** landing detection
- **LANDED:** state persistence
- **Telemetry:** $PYRO format, sequence counter, XOR checksum, state mapping, flags
