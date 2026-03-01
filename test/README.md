# Test Directory

Unit tests for the Pyro MK1B flight computer firmware.

## Framework

Uses [Unity](https://www.throwtheswitch.org/unity) test framework.

## Building Tests

```bash
cd build
cmake ..
make test_flight_controller
```

## Running Tests

```bash
./test_flight_controller
```

Or use CTest:
```bash
ctest --verbose
```

## Test Files

- `test_flight_controller.c` - Main flight controller tests
