#!/bin/bash
# Build the pyro flight computer as a WASM module.
# Requires: emsdk (https://emscripten.org/docs/getting_started/downloads.html)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT="$ROOT/docs/wasm"
mkdir -p "$OUT"

# Flight computer exports
FLIGHT_EXPORTS='
  "_sim_flight_init","_sim_flight_tick","_sim_flight_state",
  "_sim_flight_altitude_cm","_sim_flight_max_alt_cm","_sim_flight_vspeed_cms",
  "_sim_flight_pressure","_sim_flight_pyro1_fired","_sim_flight_pyro2_fired",
  "_sim_flight_armed","_sim_flight_samples","_sim_flight_launch_time",
  "_sim_flight_save_csv",
  "_sim_set_time","_sim_set_pressure","_sim_set_sensor_type",
  "_sim_set_continuity","_sim_clear_pyro_firing",
  "_sim_get_pyro_fire_count","_sim_get_pyro_last_channel",
  "_sim_get_buzzer_state","_sim_get_telemetry_len","_sim_reset",
  "_hal_fs_read_file"
'

# Physics engine exports
PHYSICS_EXPORTS='
  "_physics_wasm_init","_physics_wasm_reset","_physics_wasm_step",
  "_physics_wasm_deploy_drogue","_physics_wasm_deploy_main",
  "_physics_wasm_alt_m","_physics_wasm_vel_ms","_physics_wasm_pressure_pa",
  "_physics_wasm_apogee_m","_physics_wasm_on_ground",
  "_physics_wasm_drogue_deployed","_physics_wasm_main_deployed",
  "_physics_pressure_pa"
'

EXPORTED="[${FLIGHT_EXPORTS},${PHYSICS_EXPORTS},\"_malloc\",\"_free\"]"

emcc -O2 -s WASM=1 \
  -s "EXPORTED_FUNCTIONS=$EXPORTED" \
  -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','getValue']" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s TOTAL_MEMORY=1048576 \
  -I "$ROOT/src" \
  -I "$ROOT/sim" \
  "$ROOT/sim/main_sim.c" \
  "$ROOT/sim/hal_sim.c" \
  "$ROOT/sim/physics.c" \
  "$ROOT/src/flight_states.c" \
  "$ROOT/src/telemetry.c" \
  "$ROOT/src/telemetry_formatter.c" \
  "$ROOT/src/buzzer.c" \
  "$ROOT/src/config.c" \
  "$ROOT/src/ground_test.c" \
  -o "$OUT/pyro.js"

echo "Built: $OUT/pyro.js + $OUT/pyro.wasm"
echo "Includes: flight computer + physics engine"
