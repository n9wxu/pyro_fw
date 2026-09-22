# ── Stage 2: sources and target, added AFTER pico_sdk_init() ─────────
#
# Exports the target `pyro_board`, which the application links.

add_library(pyro_board STATIC
    hal_board.c       # implements src/board_if.h
    pyro_board.c      # implements src/pyro.h
    pressure_board.c  # implements src/pressure_sensor.h
    bmp280_driver.c   # board-private; see the header comment for why it is
                      # copied rather than shared with MK1B
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
