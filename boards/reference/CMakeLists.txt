# ── Stage 2: sources and target, added AFTER pico_sdk_init() ─────────
#
# Must export a target named `pyro_board`. Keep anything private to this
# board here -- extra drivers, compile flags, dependencies -- so the
# top-level CMakeLists.txt never needs to know this board exists.

add_library(pyro_board STATIC
    pyro_board.c      # src/pyro.h, and board_early_init()
    board_info.c      # picotool's pin names
    # One sensor on one bus. With two, write a pressure_board.c as MK1B does.
    ${CMAKE_SOURCE_DIR}/src/pressure_single_sensor.c
    # TODO: board-private drivers go here
)

target_include_directories(pyro_board PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(pyro_board PUBLIC
    pyro_hal_common # for hal.h / board_if.h headers; see src/hal_common
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_i2c
    hardware_uart
)
