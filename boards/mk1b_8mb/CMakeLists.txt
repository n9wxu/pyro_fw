# MK1B's sources, with this directory's headers ahead of MK1B's.

set(_mk1b ${CMAKE_CURRENT_LIST_DIR}/../mk1b)

add_library(pyro_board STATIC
    ${_mk1b}/pyro_board.c
    ${_mk1b}/board_info.c
    ${_mk1b}/pressure_board.c
)

target_include_directories(pyro_board PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${_mk1b})

# U5 is the base AP2192, so the check judges each channel. A definition, not
# a header line: ../mk1b/pyro_board.c finds MK1B's board_pins.h beside it.
target_compile_definitions(pyro_board PUBLIC BOARD_PYRO_U5_DISCHARGES_OUTPUTS=0)

target_link_libraries(pyro_board PUBLIC
    pyro_hal_common
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_i2c
    hardware_uart
)

if(PYRO_HAS_LUA)
    target_sources(pyro_board PRIVATE ${CMAKE_SOURCE_DIR}/src/lua/lua_pio_platform.c)
    pico_generate_pio_header(pyro_board ${CMAKE_SOURCE_DIR}/src/lua/lua_pio.pio)
    pico_generate_pio_header(pyro_board ${CMAKE_SOURCE_DIR}/src/pyro_bridge.pio)
    target_include_directories(pyro_board PRIVATE
        ${CMAKE_SOURCE_DIR}/src/lua
        ${lua_SOURCE_DIR})
    target_link_libraries(pyro_board PUBLIC hardware_pio hardware_dma)
endif()
