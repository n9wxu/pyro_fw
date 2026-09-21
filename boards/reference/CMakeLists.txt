# ── Stage 2: sources and target, added AFTER pico_sdk_init() ─────────
#
# Must export a target named `pyro_board`. Keep anything private to this
# board here -- extra drivers, compile flags, dependencies -- so the
# top-level CMakeLists.txt never needs to know this board exists.

add_library(pyro_board STATIC
    hal_board.c       # implements src/board_if.h
    pyro_board.c      # implements src/pyro.h
    pressure_board.c  # implements src/pressure_sensor.h
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
