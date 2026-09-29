# Board-independent HAL for RP2040 targets.
#
# Implements hal.h in terms of src/board_if.h. Contains no pin numbers and
# no board names, so adding a board never edits this directory.
#
# A port to a different MCU family supplies its own equivalent of this
# library; boards/<name>/CMakeLists.txt chooses which one it links.

add_library(pyro_hal_common STATIC
    hal_common.c
    board_defaults.c    # weak board_if.h defaults: plain GPIO
    ../pyro_release.c
    ../pad_claim.c
)

# The selected board's headers (board_pins.h, consumed via src/board_id.h).
# PYRO_BOARD_DIR is set by the top level and names no board, so this stays
# board-agnostic. Include-only: the board's symbols are resolved at link
# time through the application, which links both libraries.
target_include_directories(pyro_hal_common PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${PYRO_BOARD_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../..            # project root
    ${CMAKE_CURRENT_LIST_DIR}/../rtos          # rtos_tasks.h, FreeRTOSConfig.h
    ${LWIP_DIR}/src/include
    ${TINYUSB_DIR}/lib/networking
    ${PICO_SDK_PATH}/src/common/pico_usb_reset_interface_headers/include
)

target_link_libraries(pyro_hal_common PUBLIC
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_uart
    hardware_irq
    hardware_i2c
    hardware_watchdog
    littlefs_lib
    tinyusb_device            # hal_platform_init() drives tud_init/tud_task
    tinyusb_board             # bsp/board_api.h board_init()
    pico_fota_bootloader_lib  # pfb_firmware_commit() on a good boot
    FreeRTOS-Kernel-Static    # the tasks share the filesystem [DD-073]
)

# vfs.h's file handle holds a FatFs FIL on a board with an SD card, so every
# library that includes it must agree on PYRO_HAS_SD.
if(PYRO_HAS_SD)
    target_compile_definitions(pyro_hal_common PUBLIC PYRO_HAS_SD=1)
    target_include_directories(pyro_hal_common PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../../lib/fatfs
        ${CMAKE_CURRENT_LIST_DIR}/../sd)
endif()

# A flight on the bench [DD-078]: the profile's pressure in the sensor's
# place, and the channels mocked.
if(PYRO_HAS_BENCH_FLIGHT)
    target_compile_definitions(pyro_hal_common PUBLIC PYRO_HAS_BENCH_FLIGHT=1)
    target_sources(pyro_hal_common PRIVATE ../flight_sim.c ../bench_flight.c)
endif()

# A panic stamps the watchdog scratch and stops, without printing: see
# pyro_panic() in src/rtos/rtos_tasks.c.
target_compile_definitions(pyro_hal_common PUBLIC PICO_PANIC_FUNCTION=pyro_panic)
