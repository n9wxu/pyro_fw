# The board's sources, added after pico_sdk_init(), as the `pyro_board`
# library the application links.

add_library(pyro_board STATIC
    pyro_board.c      # src/pyro.h, board_early_init() and board_pyro_raw()
    pyro_measure.c    # the quiescent read and the presence test
    pyro_faults.c     # the bus's latched faults
    pyro_sequence.c   # the firing sequence
    arm_pump.c        # the ARM_TOGGLE charge pump
    board_info.c      # picotool's pin names
    ${CMAKE_SOURCE_DIR}/src/pressure_single_sensor.c  # src/pressure_sensor.h
)

target_include_directories(pyro_board PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# See THEORY_OF_OPERATION.md "Arm pump".
pico_generate_pio_header(pyro_board ${CMAKE_CURRENT_LIST_DIR}/arm_pump.pio)

# Lua's pins: shared code, this board's list in pin_caps.h.
if(PYRO_HAS_LUA)
    target_sources(pyro_board PRIVATE ${CMAKE_SOURCE_DIR}/src/lua/lua_pio_platform.c)
    pico_generate_pio_header(pyro_board ${CMAKE_SOURCE_DIR}/src/lua/lua_pio.pio)
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
