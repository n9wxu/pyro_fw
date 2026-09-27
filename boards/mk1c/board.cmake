# Settings the Pico SDK needs before pico_sdk_init(); the sources are in
# CMakeLists.txt. See boards/README.md for why a board has both.

set(PYRO_BOARD_KIND pico)
set(BOARD_DISPLAY_NAME "Pyro MK1C")

# Bare RP2040: its own header, which must not name PICO_DEFAULT_LED_PIN.
# See THEORY_OF_OPERATION.md "Pins".
set(PICO_BOARD pyro_mk1c CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/sdk)

set(PYRO_FLASH_SIZE_KB 16384) # XT25F128FWOIGT-W
set(PYRO_PFB_FS_KB     8192)  # littlefs

set(PYRO_HAS_LUA 1) # core1, on the four J3 pads, GPIO18-21

# The worst loop iteration; the watchdog is twice it. See
# THEORY_OF_OPERATION.md "Build".
set(PYRO_LOOP_WORST_MS 500)
