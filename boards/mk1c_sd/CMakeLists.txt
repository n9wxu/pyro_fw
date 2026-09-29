# MK1C's sources, with this directory's pin headers ahead of MK1C's.

set(_mk1c ${CMAKE_CURRENT_LIST_DIR}/../mk1c)

add_library(pyro_board STATIC
    ${_mk1c}/pyro_board.c
    ${_mk1c}/pyro_measure.c
    ${_mk1c}/pyro_faults.c
    ${_mk1c}/pyro_sequence.c
    ${_mk1c}/arm_pump.c
    ${_mk1c}/board_info.c
    ${CMAKE_SOURCE_DIR}/src/pressure_single_sensor.c
)

target_include_directories(pyro_board PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${_mk1c})

pico_generate_pio_header(pyro_board ${_mk1c}/arm_pump.pio)

target_link_libraries(pyro_board PUBLIC
    pyro_hal_common
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_dma
    hardware_pio
    hardware_watchdog
    hardware_i2c
    hardware_uart
    hardware_spi
)
