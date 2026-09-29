# Settings the Pico SDK needs before pico_sdk_init(). MK1C's, with J3 and
# J1.6 given to an SPI bus: an SD card and an LSM6DS3 [DD-075]. See
# THEORY_OF_OPERATION.md.

include(${CMAKE_CURRENT_LIST_DIR}/../mk1c/board.cmake)

set(BOARD_DISPLAY_NAME "Pyro MK1C-SD")

set(PYRO_HAS_LUA 0) # J3 is the SPI bus
set(PYRO_HAS_SD 1)
