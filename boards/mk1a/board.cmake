# Settings the Pico SDK needs before pico_sdk_init(); the sources are in
# CMakeLists.txt. See boards/README.md for why a board has both.

set(PYRO_BOARD_KIND pico)
set(BOARD_DISPLAY_NAME "Pyro MK1A")

# Bare RP2040 with 16 MB of flash: the stock `pico` header declares 2 MB.
set(PICO_BOARD pyro_mk1a CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

set(PYRO_FLASH_SIZE_KB 16384) # W25Q128JVS
set(PYRO_PFB_FS_KB     8192)  # littlefs

set(PYRO_HAS_LUA 1) # the two J6 pads, GPIO18/19

# The worst loop iteration; the watchdog is twice it. See
# THEORY_OF_OPERATION.md "Build".
set(PYRO_LOOP_WORST_MS 500)
