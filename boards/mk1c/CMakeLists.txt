# ── Stage 2: sources and target, added AFTER pico_sdk_init() ─────────
#
# Exports the target `pyro_board`, which the application links. Anything
# private to this board (extra drivers, compile flags, dependencies) stays
# here rather than leaking into the top-level CMakeLists.txt.

add_library(pyro_board STATIC
    hal_board.c       # implements src/board_if.h
    pyro_board.c      # implements src/pyro.h
    pressure_board.c  # implements src/pressure_sensor.h
)

target_include_directories(pyro_board PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# ARM_TOGGLE charge pump. FIFO-paced so it stops when the firmware stops;
# see the header comment in arm_pump.pio for why it is not a free-running
# PWM slice.
pico_generate_pio_header(pyro_board ${CMAKE_CURRENT_LIST_DIR}/arm_pump.pio)

# Lua user pins. The implementation is shared (src/lua/lua_pio_platform.c);
# this board contributes only its pin list, in pin_caps.h. Everything Lua can
# reach goes through PIO or plain SIO -- see the header comment in
# src/lua/lua_pio.pio for why a peripheral function is never used.
if(PYRO_HAS_LUA)
    target_sources(pyro_board PRIVATE ${CMAKE_SOURCE_DIR}/src/lua/lua_pio_platform.c)
    pico_generate_pio_header(pyro_board ${CMAKE_SOURCE_DIR}/src/lua/lua_pio.pio)
    # The half-bridge runs on the PYRO PIO, not Lua's. See pyro_bridge.pio.
    pico_generate_pio_header(pyro_board ${CMAKE_SOURCE_DIR}/src/pyro_bridge.pio)
    target_include_directories(pyro_board PRIVATE
        ${CMAKE_SOURCE_DIR}/src/lua
        ${lua_SOURCE_DIR})
    target_link_libraries(pyro_board PUBLIC hardware_pio hardware_dma)
endif()

target_link_libraries(pyro_board PUBLIC
    pyro_hal_common # for hal.h / board_if.h headers; see src/hal_common
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_dma
    hardware_pio
    hardware_watchdog
    hardware_i2c
    hardware_uart
)

# Bench mode: hold BIAS_BUS continuously so a multimeter can read a steady DC
# level on the firing bus, instead of the ~3% duty pulse the routine tracking
# test uses. Firing side stays cold -- FIRE_A/FIRE_B/ARM_TOGGLE untouched.
#
#   cmake -B build-hold -DPYRO_BOARD=mk1c -DPYRO_MK1C_BIAS_HOLD=ON
#
# Must be OFF for flight.
option(PYRO_MK1C_BIAS_HOLD "Bench: hold BIAS_BUS continuously" OFF)
if(PYRO_MK1C_BIAS_HOLD)
    target_compile_definitions(pyro_board PRIVATE PYRO_MK1C_BIAS_HOLD=1)
    message(STATUS "  MK1C BENCH MODE: BIAS_BUS held continuously (not for flight)")
endif()
