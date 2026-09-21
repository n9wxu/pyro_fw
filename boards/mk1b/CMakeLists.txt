# ── Stage 2: sources and target, added AFTER pico_sdk_init() ─────────
#
# Exports the target `pyro_board`, which the application links. Anything
# private to this board (extra drivers, compile flags, dependencies) stays
# here rather than leaking into the top-level CMakeLists.txt.

add_library(pyro_board STATIC
    hal_board.c       # implements src/board_if.h
    pyro_board.c      # implements src/pyro.h
    pressure_board.c  # implements src/pressure_sensor.h
    bmp280_driver.c   # MK1B only; MK1C has no BMP280 fitted
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
