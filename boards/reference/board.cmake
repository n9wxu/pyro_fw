# ── Stage 1: settings needed BEFORE pico_sdk_init() ──────────────────
#
# Included by the top-level CMakeLists.txt before the SDK is initialised.
# PICO_BOARD and PICO_BOARD_HEADER_DIRS are consumed during SDK init, and
# the flash geometry is consumed by pico_fota_bootloader when it generates
# its linker script at FetchContent time -- all before add_subdirectory()
# would run. That is why a board has two CMake entry points.
#
# The top level requires BOARD_DISPLAY_NAME, PYRO_FLASH_SIZE_KB and
# PYRO_PFB_FS_KB, and fails the configure if any is missing.

set(PYRO_BOARD_KIND pico)  # RP2040 target: uses the Pico SDK and src/hal_common
set(BOARD_DISPLAY_NAME "Pyro REFERENCE")  # TODO: match BOARD_NAME_STR

# TODO: a stock Pico module can use the SDK's own header:
#           set(PICO_BOARD pico CACHE STRING "Board type" FORCE)
#       A bare RP2040 needs its own. If you write one, see the warning in
#       boards/mk1c/sdk/pyro_mk1c.h about PICO_DEFAULT_LED_PIN and about
#       pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, ...) -- omitting
#       the latter produces a NEGATIVE flash size that still links.
set(PICO_BOARD pico CACHE STRING "Board type" FORCE)
# list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

# TODO: must match PICO_FLASH_SIZE_BYTES for this board.
set(PYRO_FLASH_SIZE_KB 2048)

# TODO: littlefs reservation, carved off the top of flash.
# The top level checks that (flash - fs - 40) is a multiple of 8, so the
# bootloader's A/B slots land on a 4k boundary. A bad value fails the
# configure with the arithmetic shown.
set(PYRO_PFB_FS_KB 984)
