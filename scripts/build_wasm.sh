#!/bin/bash
# Build the pyro flight computer as a WASM module.
#
# The WASM/host HAL is a board like any other: boards/sim/. Select it with
#     cmake -B build-sim -DPYRO_BOARD=sim
# for the native simulator; this script drives emcc directly.
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

# Lua user programs: the VM host plus the simulated platform, so the browser
# can run a script, see its pin effects and talk to its UART.
LUA_EXPORTS='
  "_pyro_lua_init","_pyro_lua_shutdown","_pyro_lua_load","_pyro_lua_tick",
  "_pyro_lua_event","_pyro_lua_eval","_pyro_lua_last_error",
  "_sim_lua_set_flight","_sim_lua_set_pyro","_sim_lua_set_input",
  "_sim_lua_output_count","_sim_lua_output_name","_sim_lua_output_value",
  "_sim_lua_output_dimmable",
  "_sim_lua_input_count","_sim_lua_input_name",
  "_sim_lua_serial_count","_sim_lua_serial_name",
  "_sim_lua_uart_tx","_sim_lua_uart_tx_clear","_sim_lua_uart_rx_push",
  "_sim_lua_console","_sim_lua_console_clear",
  "_sim_lua_pixel_count","_sim_lua_pixel_r","_sim_lua_pixel_g","_sim_lua_pixel_b",
  "_sim_lua_pixel_shows"
'

EXPORTED="[${FLIGHT_EXPORTS},${PHYSICS_EXPORTS},${LUA_EXPORTS},\"_malloc\",\"_free\"]"

# Lua sources. Fetched once into build-wasm-lua/ and reused; the same four
# libraries are excluded as in the CMake build -- see the note there.
LUA_DIR="$ROOT/build-wasm-lua/lua"
if [ ! -d "$LUA_DIR" ]; then
    mkdir -p "$ROOT/build-wasm-lua"
    git clone --depth 1 --branch v5.4.6 https://github.com/lua/lua.git "$LUA_DIR"
fi
LUA_SRC=""
for f in lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
         lopcodes lparser lstate lstring ltable ltm lundump lvm lzio \
         lauxlib lbaselib lcorolib ldblib lmathlib lstrlib ltablib lutf8lib; do
    LUA_SRC="$LUA_SRC $LUA_DIR/$f.c"
done

emcc -O2 -s WASM=1 \
  -DLUA_USER_H='"pyro_luaconf.h"' \
  -Wno-implicit-fallthrough -Wno-unused-but-set-variable \
  -s "EXPORTED_FUNCTIONS=$EXPORTED" \
  -s "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','getValue']" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s TOTAL_MEMORY=1048576 \
  -I "$ROOT/src" \
  -I "$ROOT/sim" \
  -I "$ROOT/boards/sim" \
  -I "$ROOT/src/lua" \
  -I "$ROOT/build-wasm-lua/lua" \
  "$ROOT/sim/main_sim.c" \
  "$ROOT/boards/sim/hal_sim.c" \
  "$ROOT/sim/physics.c" \
  "$ROOT/src/flight_states.c" \
  "$ROOT/src/pressure_processing.c" \
  "$ROOT/src/telemetry.c" \
  "$ROOT/src/telemetry_formatter.c" \
  "$ROOT/src/buzzer.c" \
  "$ROOT/src/config.c" \
  "$ROOT/src/ground_test.c" \
  "$ROOT/src/lua/pyro_lua.c" \
  "$ROOT/src/lua/lua_arena.c" \
  "$ROOT/boards/sim/lua_platform_sim.c" \
  $LUA_SRC \
  -o "$OUT/pyro.js"

echo "Built: $OUT/pyro.js + $OUT/pyro.wasm"
echo "Includes: flight computer + physics engine"
