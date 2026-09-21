# Board-independent HAL for RP2040 targets.
#
# Implements hal.h in terms of src/board_if.h. Contains no pin numbers and
# no board names, so adding a board never edits this directory.
#
# A port to a different MCU family supplies its own equivalent of this
# library; boards/<name>/CMakeLists.txt chooses which one it links.

add_library(pyro_hal_common STATIC
    hal_common.c
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
)
